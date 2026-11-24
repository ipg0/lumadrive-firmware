#include "matrix_service.hpp"
#include "matrix_core.hpp"
#include "matrix_wire.hpp"
#include "modulation.hpp"
#include "../signal_source/source_factory.hpp"
#include "signal_buffer.hpp"
#include "memory_stress_test.hpp"
#include "default_matrix.hpp"
#include "../lighting_driver/impl/linear_lighting_driver_view.hpp"
#include "../connected_lighting_drivers/connected_lighting_drivers.hpp"
#include "../hardware_config/hardware_config.hpp"
#include "../global_config/global_config.hpp"
#include "../master_clock_manager/master_clock_manager.hpp"
#include "../mesh_interop/mesh_interop.hpp"
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <new>
#include <type_traits>
#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_now.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs.h>
#include <pb_decode.h>
#include <pb_encode.h>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

namespace matrix_service {
namespace {
using namespace matrix;
namespace w = matrix::wire;
constexpr uint32_t RECORD_VERSION = 0;
// Give paired outputs time to receive the first heartbeat after a master boot.
// Offline nodes must not prevent the rest of the installation from starting.
constexpr uint64_t STANDBY_TIMEOUT_US = 5000000;
constexpr uint32_t IDENTIFY_ALL_DRIVERS = UINT32_MAX;
// Decode an editor patch directly into the replacement matrix. Keeping a
// maximum-sized LayoutUpdate/MatrixUpdate alongside it exhausts fragmented RAM
// even for tiny edits. Nanopb still decodes and bounds every nested message.
[[maybe_unused]] bool decode_editor_update(const std::vector<uint8_t>& bytes, bool layout,
                                           matrix_config& config, uint64_t& expected_revision,
                                           uint64_t& master_session) {
  if (bytes.size() > MAX_BYTES)
    return false;
  expected_revision = master_session = 0;
  if (layout) {
    config.layout_count = 0;
    config.has_center_index = false;
    config.center_index = 0;
  } else {
    config.effects_count = config.sources_count = config.conditions_count = config.entries_count =
        0;
    config.modes_count = 0;
    config.signals_count = 0;
    config.active_mode_id = 0;
  }
  auto stream = pb_istream_from_buffer(bytes.data(), bytes.size());
  while (stream.bytes_left) {
    pb_wire_type_t wire;
    uint32_t tag;
    bool eof;
    if (!pb_decode_tag(&stream, &wire, &tag, &eof) || !tag)
      return false;
    if (tag == 1 || tag == 2) {
      if (wire != PB_WT_VARINT ||
          !pb_decode_varint(&stream, tag == 1 ? &expected_revision : &master_session))
        return false;
      continue;
    }
    if (!layout && tag == mesh_interop_MatrixUpdate_active_mode_id_tag) {
      uint64_t value;
      if (wire != PB_WT_VARINT || !pb_decode_varint(&stream, &value) || value > UINT32_MAX)
        return false;
      config.active_mode_id = value;
      continue;
    }
    if (layout && tag == mesh_interop_LayoutUpdate_center_index_tag) {
      uint64_t value;
      if (wire != PB_WT_VARINT || !pb_decode_varint(&stream, &value) || value > UINT32_MAX)
        return false;
      config.has_center_index = true;
      config.center_index = value;
      continue;
    }
    const auto repeated = [&](const pb_msgdesc_t* fields, auto& values, auto& count) {
      if (wire != PB_WT_STRING || count >= std::size(values))
        return false;
      pb_istream_t message;
      if (!pb_make_string_substream(&stream, &message))
        return false;
      if (!pb_decode(&message, fields, &values[count]) ||
          !pb_close_string_substream(&stream, &message))
        return false;
      ++count;
      return true;
    };
    if (layout && tag == mesh_interop_LayoutUpdate_layout_tag) {
      if (!repeated(mesh_interop_MatrixLayout_fields, config.layout, config.layout_count))
        return false;
    } else if (!layout && tag == mesh_interop_MatrixUpdate_effects_tag) {
      if (!repeated(mesh_interop_MatrixEffect_fields, config.effects, config.effects_count))
        return false;
    } else if (!layout && tag == mesh_interop_MatrixUpdate_sources_tag) {
      if (!repeated(mesh_interop_MatrixSource_fields, config.sources, config.sources_count))
        return false;
    } else if (!layout && tag == mesh_interop_MatrixUpdate_conditions_tag) {
      if (!repeated(mesh_interop_MatrixCondition_fields, config.conditions,
                    config.conditions_count))
        return false;
    } else if (!layout && tag == mesh_interop_MatrixUpdate_entries_tag) {
      if (!repeated(mesh_interop_MatrixEntry_fields, config.entries, config.entries_count))
        return false;
    } else if (!layout && tag == mesh_interop_MatrixUpdate_modes_tag) {
      if (!repeated(mesh_interop_MatrixMode_fields, config.modes, config.modes_count))
        return false;
    } else if (!layout && tag == mesh_interop_MatrixUpdate_signals_tag) {
      if (!repeated(mesh_interop_SignalGenerator_fields, config.signals, config.signals_count))
        return false;
    } else if (!pb_skip_field(&stream, wire))
      return false;
  }
  return true;
}
// Decode editor patches into populated vectors. The full Nanopb config has
// fixed storage for every possible effect and output, even for a tiny update.
bool decode_compact_editor_update(const std::vector<uint8_t>& bytes, bool layout,
                                  runtime_config& config, uint64_t& expected_revision,
                                  uint64_t& master_session) {
  if (bytes.size() > MAX_BYTES)
    return false;
  expected_revision = master_session = 0;
  if (layout) {
    config.layout.clear();
    config.has_center_index = false;
    config.center_index = 0;
  } else {
    config.effects.clear();
    config.sources.clear();
    config.conditions.clear();
    config.entries.clear();
    config.modes.clear();
    config.signals.clear();
    config.active_mode_id = 0;
  }
  auto stream = pb_istream_from_buffer(bytes.data(), bytes.size());
  while (stream.bytes_left) {
    pb_wire_type_t wire;
    uint32_t tag;
    bool eof;
    if (!pb_decode_tag(&stream, &wire, &tag, &eof) || !tag)
      return false;
    if (tag == 1 || tag == 2) {
      if (wire != PB_WT_VARINT ||
          !pb_decode_varint(&stream, tag == 1 ? &expected_revision : &master_session))
        return false;
      continue;
    }
    if (!layout && tag == mesh_interop_MatrixUpdate_active_mode_id_tag) {
      uint64_t value;
      if (wire != PB_WT_VARINT || !pb_decode_varint(&stream, &value) || value > UINT32_MAX)
        return false;
      config.active_mode_id = value;
      continue;
    }
    if (layout && tag == mesh_interop_LayoutUpdate_center_index_tag) {
      uint64_t value;
      if (wire != PB_WT_VARINT || !pb_decode_varint(&stream, &value) || value > UINT32_MAX)
        return false;
      config.has_center_index = true;
      config.center_index = value;
      continue;
    }
    const auto repeated = [&](const pb_msgdesc_t* fields, auto& values, size_t limit) {
      if (wire != PB_WT_STRING || values.size() >= limit)
        return false;
      typename std::decay_t<decltype(values)>::value_type item{};
      pb_istream_t message;
      if (!pb_make_string_substream(&stream, &message) || !pb_decode(&message, fields, &item) ||
          !pb_close_string_substream(&stream, &message))
        return false;
      values.push_back(item);
      return true;
    };
    if (layout && tag == mesh_interop_LayoutUpdate_layout_tag) {
      if (!repeated(mesh_interop_MatrixLayout_fields, config.layout, MAX_LAYOUT_SOURCES))
        return false;
    } else if (!layout && tag == mesh_interop_MatrixUpdate_effects_tag) {
      if (!repeated(mesh_interop_MatrixEffect_fields, config.effects, 32))
        return false;
    } else if (!layout && tag == mesh_interop_MatrixUpdate_sources_tag) {
      if (!repeated(mesh_interop_MatrixSource_fields, config.sources, 32))
        return false;
    } else if (!layout && tag == mesh_interop_MatrixUpdate_conditions_tag) {
      if (!repeated(mesh_interop_MatrixCondition_fields, config.conditions, 32))
        return false;
    } else if (!layout && tag == mesh_interop_MatrixUpdate_entries_tag) {
      if (!repeated(mesh_interop_MatrixEntry_fields, config.entries, 32))
        return false;
    } else if (!layout && tag == mesh_interop_MatrixUpdate_modes_tag) {
      if (!repeated(mesh_interop_MatrixMode_fields, config.modes, 32))
        return false;
    } else if (!layout && tag == mesh_interop_MatrixUpdate_signals_tag) {
      if (!repeated(mesh_interop_SignalGenerator_fields, config.signals, 32))
        return false;
    } else if (!pb_skip_field(&stream, wire))
      return false;
  }
  config.effects_count = config.effects.size();
  config.sources_count = config.sources.size();
  config.conditions_count = config.conditions.size();
  config.entries_count = config.entries.size();
  config.layout_count = config.layout.size();
  config.modes_count = config.modes.size();
  config.signals_count = config.signals.size();
  return true;
}
bool decode_compact_config(const uint8_t* bytes, size_t size, runtime_config& config) {
  if (size > MAX_BYTES)
    return false;
  auto stream = pb_istream_from_buffer(bytes, size);
  while (stream.bytes_left) {
    pb_wire_type_t wire;
    uint32_t tag;
    bool eof;
    if (!pb_decode_tag(&stream, &wire, &tag, &eof) || !tag)
      return false;
    if (tag == mesh_interop_MatrixConfig_schema_version_tag ||
        tag == mesh_interop_MatrixConfig_factory_default_tag ||
        tag == mesh_interop_MatrixConfig_active_mode_id_tag ||
        tag == mesh_interop_MatrixConfig_center_index_tag) {
      uint64_t value;
      if (wire != PB_WT_VARINT || !pb_decode_varint(&stream, &value) || value > UINT32_MAX)
        return false;
      if (tag == mesh_interop_MatrixConfig_schema_version_tag)
        config.schema_version = value;
      else if (tag == mesh_interop_MatrixConfig_factory_default_tag)
        config.factory_default = value != 0;
      else if (tag == mesh_interop_MatrixConfig_center_index_tag) {
        config.has_center_index = true;
        config.center_index = value;
      } else
        config.active_mode_id = value;
      continue;
    }
    const auto repeated = [&](const pb_msgdesc_t* fields, auto& values, size_t limit) {
      if (wire != PB_WT_STRING || values.size() >= limit)
        return false;
      typename std::decay_t<decltype(values)>::value_type item{};
      pb_istream_t message;
      if (!pb_make_string_substream(&stream, &message) || !pb_decode(&message, fields, &item) ||
          !pb_close_string_substream(&stream, &message))
        return false;
      values.push_back(item);
      return true;
    };
    if (tag == mesh_interop_MatrixConfig_effects_tag) {
      if (!repeated(mesh_interop_MatrixEffect_fields, config.effects, 32))
        return false;
    } else if (tag == mesh_interop_MatrixConfig_sources_tag) {
      if (!repeated(mesh_interop_MatrixSource_fields, config.sources, 32))
        return false;
    } else if (tag == mesh_interop_MatrixConfig_conditions_tag) {
      if (!repeated(mesh_interop_MatrixCondition_fields, config.conditions, 32))
        return false;
    } else if (tag == mesh_interop_MatrixConfig_entries_tag) {
      if (!repeated(mesh_interop_MatrixEntry_fields, config.entries, 32))
        return false;
    } else if (tag == mesh_interop_MatrixConfig_layout_tag) {
      if (!repeated(mesh_interop_MatrixLayout_fields, config.layout, MAX_LAYOUT_SOURCES))
        return false;
    } else if (tag == mesh_interop_MatrixConfig_modes_tag) {
      if (!repeated(mesh_interop_MatrixMode_fields, config.modes, 32))
        return false;
    } else if (tag == mesh_interop_MatrixConfig_signals_tag) {
      if (!repeated(mesh_interop_SignalGenerator_fields, config.signals, 32))
        return false;
    } else if (!pb_skip_field(&stream, wire))
      return false;
  }
  config.effects_count = config.effects.size();
  config.sources_count = config.sources.size();
  config.conditions_count = config.conditions.size();
  config.entries_count = config.entries.size();
  config.layout_count = config.layout.size();
  config.modes_count = config.modes.size();
  config.signals_count = config.signals.size();
  return true;
}
bool decode_compact_config(const std::vector<uint8_t>& bytes, runtime_config& config) {
  return decode_compact_config(bytes.data(), bytes.size(), config);
}
constexpr auto STAGE_RECEIVING = mesh_interop_MatrixStatus_Stage_RECEIVING;
constexpr auto STAGE_FAILED = mesh_interop_MatrixStatus_Stage_FAILED;
constexpr auto STAGE_DISTRIBUTING = mesh_interop_MatrixStatus_Stage_DISTRIBUTING;
constexpr auto STAGE_ACTIVE = mesh_interop_MatrixStatus_Stage_ACTIVE;
constexpr auto NODE_READY = mesh_interop_MatrixNodeStatus_State_READY;
constexpr auto NODE_PREPARING = mesh_interop_MatrixNodeStatus_State_PREPARING;
constexpr auto NODE_UNAVAILABLE = mesh_interop_MatrixNodeStatus_State_UNAVAILABLE;
constexpr auto NODE_DEGRADED = mesh_interop_MatrixNodeStatus_State_DEGRADED;
struct packet {
  uint8_t mac[6]{};
  uint16_t size = 0, connection = 0;
  uint32_t generation = 0;
  uint8_t data[512]{};
};
struct signal_packet {
  uint8_t mac[6]{};
  uint16_t size = 0;
  uint8_t data[w::PACKET_LIMIT]{};
};
struct gpio_event {
  uint64_t at;
  uint32_t pin;
  bool rising;
};
struct pending_gpio_event {
  gpio_event edge;
  uint64_t revision;
  uint32_t sequence;
};
QueueHandle_t radio_queue = nullptr, priority_queue = nullptr, ble_queue = nullptr,
              gpio_queue = nullptr;
QueueHandle_t sensor_queue = nullptr;
QueueHandle_t signal_queue = nullptr;
TaskHandle_t worker_task = nullptr;
std::atomic<uint32_t> ble_generation{0};
std::atomic<bool> overflow{false};
std::atomic<bool> gpio_overflow{false};
std::atomic<bool> initialized{false};
std::atomic<void (*)()> notify{nullptr};
std::mutex status_mutex;
mesh_interop_MatrixStatus published{};
std::array<uint8_t, 512> published_bytes{};
size_t published_size = 0;
std::array<uint8_t, 512> layout_response_bytes{0x08, 0};
size_t layout_response_size = 2;

struct peer {
  uint8_t mac[6]{};
  uint64_t seen = 0, last_retry = 0, last_recover = 0;
  bool definitions = false, ready = false, snapshotting = false;
  bool replacement_prepared = false;
  bool attempted = false;
  bool preview_attempted = false;
  uint32_t sequence = 0;
  unsigned retries = 0;
  std::deque<animation_command> commands;
  std::deque<animation_command> retiring_commands;
  uint32_t retiring_sequence = 0;
  uint64_t retiring_retry = 0;
  unsigned retiring_retries = 0;
  uint64_t gpio_stream = 0;
  std::array<uint64_t, 4> retired_gpio_streams{};
  size_t retired_gpio_index = 0;
  uint32_t gpio_sequence = 0;
  bool gpio_degraded = false;
  bool catalog_known = false;
  size_t outputs_count = 0;
  std::array<mesh_interop_LayoutOutput, MAX_SOURCES_PER_NODE> outputs{};
  uint64_t catalog_at = 0, catalog_retry = 0;
  uint64_t gpio_mask = 0;
  uint64_t gpio_known = 0, gpio_high = 0, gpio_state_at = 0, gpio_snapshot_seen = 0,
           gpio_request_at = 0;
  char firmware_version[32]{};
};
struct outbound {
  int peer_index = -1;
  uint8_t kind = 0, phase = 0;
  uint32_t id = 0, checksum = 0;
  size_t offset = 0, sent_end = 0;
  uint64_t last_send = 0;
  unsigned retries = 0;
  std::vector<uint8_t> bytes;
};
class service {
public:
  bool master = false;
  uint8_t mac[6]{};
  uint64_t session = 0, revision = 0, activate_at = 0, last_hello = 0;
  uint32_t local_sequence = 0, completed_transfer = 0;
  uint32_t previous_sequence = 0;
  uint32_t upload_generation = 0;
  uint16_t upload_connection = 0;
  uint8_t inbound_kind = 0;
  uint64_t inbound_revision = 0;
  bool deployment = false;
  bool default_layout_dirty = false;
  bool configuration_persisted = false, active_persisted = false;
  bool upload_is_layout = false;
  bool upload_is_matrix = false;
  bool snapshot_is_matrix = false;
  bool snapshot_is_active = false;
  std::vector<uint8_t> layout_snapshot_bytes;
  std::vector<mesh_interop_LayoutOutput> snapshot_outputs;
  uint32_t layout_snapshot_id = 0, layout_generation = 0;
  uint16_t layout_connection = 0;
  uint64_t layout_touched = 0;
  uint32_t identify_driver = 0, last_identify_id = 0, last_identify_driver = 0;
  uint32_t identify_source = 0, last_identify_source = 0;
  uint32_t identify_length = 0, last_identify_length = 0;
  bool identify_reversed = false, last_identify_reversed = false;
  uint64_t identify_until = 0;
  bool last_identify_ok = false;
  struct identify_request {
    bool active = false;
    int peer_index = -1;
    uint32_t request_id = 0, wire_id = 0, driver = 0, source = 0, generation = 0, length = 0;
    bool reversed = false;
    unsigned retries = 0;
    uint64_t sent_at = 0;
  } identifying;
  bool configured_pins[40]{};
  uint64_t gpio_stream = 0, gpio_last_send = 0;
  uint32_t gpio_sequence = 0;
  unsigned gpio_retries = 0;
  bool gpio_degraded = false;
  uint64_t gpio_snapshot_at = 0, gpio_snapshot_last_send = 0, gpio_snapshot_acked = 0,
           gpio_snapshot_started = 0;
  std::vector<uint8_t> gpio_snapshot;
  uint64_t startup_at = 0, local_gpio_known = 0, local_gpio_high = 0;
  std::deque<pending_gpio_event> gpio_events;
  heap_object<runtime_config> config, candidate;
  heap_object<runtime_config> retiring_config;
  heap_object<player> ledger, local, pending_player;
  heap_object<player> preview_player;
  uint64_t preview_start = 0, preview_end = 0;
  std::vector<uint8_t> preview_bytes;
  bool preview_distributing = false;
  signal_buffer signal_values;
  heap_object<signal_source::source_registry> signal_sources;
  bool signal_memory_error = false;
  uint64_t last_signal_batch = 0;
  uint64_t last_sensor_batch = 0;
  struct sensor_value {
    uint8_t mac[6]{};
    uint32_t id = 0;
    uint64_t time = 0;
    float value = 0;
  };
  std::array<sensor_value, 32> sensor_values{};
  heap_object<player> prepared_ledger, prepared_local;
  heap_object<player> retiring_ledger;
  heap_object<animation_snapshot> pending_snapshot;
  evaluator rules;
  bool boot_pending = true;
  uint64_t slave_started_at = 0;
  uint64_t standby_last_sync = 0;
  uint64_t discovery_last_sync = 0;
  bool standby = false;
  bool discovering = false;
  bool waiting_for_discovery_sync = false;
  evaluator retiring_rules;
  receiver ble_rx, radio_rx;
  std::array<peer, 16> peers{};
  size_t peer_count = 0;
  outbound tx;
  mesh_interop_MatrixStatus state{};

  void error(const char* message, bool fatal = false) {
    snprintf(state.error, sizeof(state.error), "%s", message);
    if (fatal)
      state.stage = STAGE_FAILED;
    ESP_LOGW("matrix", "%s", message);
  }
  void log_memory() const {
#ifdef ESP_PLATFORM
    ESP_LOGI("matrix",
             "Free heap: %u largest: %u; matrix stack headroom: %u; player: %u; service: %u; "
             "config: %u bytes",
             unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
             unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
             unsigned(uxTaskGetStackHighWaterMark(nullptr)), unsigned(sizeof(player)),
             unsigned(sizeof(service)), unsigned(sizeof(matrix_config)));
#endif
  }
  void publish() {
    bool changed;
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      changed = memcmp(&published, &state, sizeof(state)) != 0;
      if (changed || !published_size) {
        auto bytes = encode(mesh_interop_MatrixStatus_fields, &state);
        if (bytes.size() <= published_bytes.size()) {
          memcpy(published_bytes.data(), bytes.data(), bytes.size());
          published_size = bytes.size();
          published = state;
        }
      }
    }
    if (changed) {
      auto cb = notify.load();
      if (cb)
        cb();
    }
  }
  void send(const uint8_t* dest, w::type type, const std::vector<uint8_t>& body) {
    auto p = w::packet(type, body);
    if (!p.empty())
      esp_now_send(dest, p.data(), p.size());
  }
  int find_peer(const uint8_t* address) {
    for (size_t i = 0; i < peer_count; ++i)
      if (!memcmp(peers[i].mac, address, 6))
        return i;
    if (peer_count == peers.size())
      return -1;
    memcpy(peers[peer_count].mac, address, 6);
    return peer_count++;
  }
  bool available(const peer& p, uint64_t now) const {
    return p.seen && (now <= p.seen || now - p.seen < 4000000);
  }
  static bool lookup_signal(void* arg, uint64_t rev, uint32_t id, uint64_t clock, float& value) {
    return static_cast<service*>(arg)->signal_values.lookup(rev, id, clock, value);
  }
  void accept_sensor_value(const uint8_t* owner, uint32_t id, uint64_t time, float value,
                           uint64_t clock) {
    if (!id || !std::isfinite(value) || time > clock + 100000 ||
        (clock > time && clock - time > 250000))
      return;
    sensor_value* entry = nullptr;
    for (auto& candidate : sensor_values)
      if (candidate.id == id && !memcmp(candidate.mac, owner, 6)) {
        entry = &candidate;
        break;
      }
    if (!entry)
      for (auto& candidate : sensor_values)
        if (!candidate.id) {
          entry = &candidate;
          break;
        }
    if (!entry || (entry->id && time <= entry->time))
      return;
    memcpy(entry->mac, owner, 6);
    entry->id = id;
    entry->time = time;
    entry->value = value;
  }
  void accept_sensor_batch(const uint8_t* owner, const uint8_t* body, size_t size, uint64_t clock) {
    if (size < 9 || w::get(body, 8) != session || !body[8] || body[8] > 14 ||
        size != 9 + size_t(body[8]) * 16)
      return;
    for (size_t i = 0; i < body[8]; ++i) {
      const auto* item = body + 9 + i * 16;
      const auto id = uint32_t(w::get(item, 4)), bits = uint32_t(w::get(item + 12, 4));
      float value;
      memcpy(&value, &bits, sizeof(value));
      accept_sensor_value(owner, id, w::get(item + 4, 8), value, clock);
    }
  }
  void collect_sensor_samples(uint64_t clock) {
    if (!sensor_queue || !master_clock_manager::instance().is_synchronized())
      return;
    const auto now = uint64_t(esp_timer_get_time());
    if (last_sensor_batch && now - last_sensor_batch < 16667)
      return;
    last_sensor_batch = now;
    std::vector<uint8_t> body;
    w::put(body, session, 8);
    w::put(body, 0, 1);
    signal_source::sample sample{};
    uint8_t count = 0;
    while (count < 14 && xQueueReceive(sensor_queue, &sample, 0) == pdTRUE) {
      const auto timestamp =
          master_clock_manager::instance().get_master_time_at(sample.captured_local_us);
      if (timestamp < 0)
        continue;
      if (master)
        accept_sensor_value(mac, sample.sensor_id, timestamp, sample.value, clock);
      else {
        uint32_t bits;
        memcpy(&bits, &sample.value, sizeof(bits));
        w::put(body, sample.sensor_id, 4);
        w::put(body, timestamp, 8);
        w::put(body, bits, 4);
        ++count;
      }
    }
    if (!master && count) {
      const auto mesh = mesh_interop::instance().get_state();
      if (mesh.nodes_count) {
        body[8] = count;
        send(mesh.nodes[0].mac, w::SENSOR_BATCH, body);
      }
    }
  }
  void accept_signal_batch(const uint8_t* body, size_t size, uint64_t clock) {
    if (size < 25 || w::get(body, 8) != session)
      return;
    const auto rev = w::get(body + 8, 8), timestamp = w::get(body + 16, 8);
    const auto count = body[24];
    if (!count || count > 27 || size != 25 + size_t(count) * 8 || timestamp > clock + 150000 ||
        (clock > timestamp && clock - timestamp > 250000))
      return;
    const bool valid_revision =
        (local && rev == local->definitions().revision) ||
        (pending_player && rev == pending_player->definitions().revision) ||
        (master && ledger && rev == ledger->definitions().revision) ||
        (master && retiring_ledger && rev == retiring_ledger->definitions().revision);
    if (!valid_revision)
      return;
    for (size_t i = 0; i < count; ++i) {
      const auto id = uint32_t(w::get(body + 25 + i * 8, 4));
      const auto bits = uint32_t(w::get(body + 29 + i * 8, 4));
      float value;
      memcpy(&value, &bits, sizeof(value));
      if (!id || !std::isfinite(value))
        continue;
      if (!signal_values.update(rev, id, timestamp, value)) {
        if (!signal_memory_error)
          error("Unable to allocate signal buffer");
        signal_memory_error = true;
      } else
        signal_memory_error = false;
    }
  }
  static bool read_sensor_source(void* context, const uint8_t* owner, uint32_t id, uint64_t clock,
                                 float& value) {
    const auto& values = static_cast<service*>(context)->sensor_values;
    const auto found = std::find_if(values.begin(), values.end(), [&](const auto& entry) {
      return entry.id == id && !memcmp(entry.mac, owner, 6) && clock >= entry.time &&
             clock - entry.time <= signal_buffer::STALE_US;
    });
    if (found == values.end())
      return false;
    value = found->value;
    return true;
  }
  signal_source::source* find_signal_source(const mesh_interop_SignalGenerator& config,
                                            uint64_t rev) {
    if (!signal_sources)
      signal_sources = make_heap_object<signal_source::source_registry>();
    if (!signal_sources)
      return nullptr;
    if (auto* existing = signal_sources->find(rev, config.id))
      return existing;
    signal_source::source_config definition{};
    if (config.kind == mesh_interop_SignalGenerator_Kind_SENSOR) {
      definition.kind = signal_source::source_kind::REMOTE_SENSOR;
      memcpy(definition.node_mac, config.node_mac, 6);
      definition.sensor_id = config.sensor_id;
    } else {
      definition.kind = signal_source::source_kind::VIRTUAL;
      definition.generator = {static_cast<signal_source::waveform>(
                                  config.kind - mesh_interop_SignalGenerator_Kind_SINE),
                              config.period_ms, config.amplitude, config.offset, config.phase};
    }
    auto source = signal_source::make_configured_source(definition, read_sensor_source, this);
    if (!signal_sources->install(rev, config.id, std::move(source)))
      return nullptr;
    return signal_sources->find(rev, config.id);
  }
  void send_signal_batches(const runtime_config& matrix, uint64_t rev, uint64_t clock) {
    const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    size_t index = 0;
    while (index < matrix.signals_count) {
      std::vector<uint8_t> body;
      w::put(body, session, 8);
      w::put(body, rev, 8);
      w::put(body, clock + 30000, 8);
      w::put(body, 0, 1);
      uint8_t count = 0;
      while (index < matrix.signals_count && count < 27) {
        const auto& signal = matrix.signals[index++];
        float value = 0;
        auto* source = find_signal_source(signal, rev);
        if (!source) {
          if (!signal_memory_error)
            error("Unable to allocate signal source");
          signal_memory_error = true;
          continue;
        }
        if (!source->sample(clock, value))
          continue;
        uint32_t bits;
        memcpy(&bits, &value, sizeof(bits));
        w::put(body, signal.id, 4);
        w::put(body, bits, 4);
        ++count;
      }
      if (!count)
        continue;
      body[24] = count;
      accept_signal_batch(body.data(), body.size(), clock);
      send(broadcast, w::SIGNAL_BATCH, body);
    }
  }
  template <typename config_type>
  const char* platform_validate(const config_type& c, bool check_peers) {
    if (auto e = validate(c, mac))
      return e;
    for (size_t i = 0; i < c.layout_count; ++i) {
      const auto& m = c.layout[i];
      if (!memcmp(m.node_mac, mac, 6)) {
        auto* driver = connected_lighting_drivers::find(m.driver);
        if (!driver) {
          ESP_LOGE("matrix", "Layout references missing local driver %u", unsigned(m.driver));
          return "Layout references a missing local driver";
        }
        if (m.source_index >= driver->get_layout_source_count()) {
          ESP_LOGE("matrix", "Layout references missing source %u on local driver %u",
                   unsigned(m.source_index), unsigned(m.driver));
          return "Layout references a missing local driver source";
        }
        const auto span = std::max(m.from, m.to) - std::min(m.from, m.to);
        const auto maximum = driver->get_layout_max_pixel_count(m.source_index);
        if (span > maximum) {
          ESP_LOGE("matrix", "Local driver %u source %u layout length %u exceeds maximum %u",
                   unsigned(m.driver), unsigned(m.source_index), unsigned(span), unsigned(maximum));
          return "Layout span exceeds local driver capacity";
        }
      } else if (check_peers && !mesh_interop::instance().slave_node_exists(m.node_mac))
        return "Layout node is not paired";
      else {
        const auto index = find_peer(m.node_mac);
        if (index >= 0 && peers[index].catalog_known) {
          const auto& p = peers[index];
          bool found = false;
          for (size_t j = 0; j < p.outputs_count; ++j)
            if (p.outputs[j].driver == m.driver && p.outputs[j].source_index == m.source_index &&
                p.outputs[j].max_pixel_count >= std::max(m.from, m.to) - std::min(m.from, m.to))
              found = true;
          if (!found)
            return "Layout length exceeds reported hardware capacity";
        }
      }
    }
    for (size_t i = 0; i < c.sources_count; ++i) {
      auto& s = c.sources[i];
      if (s.kind != mesh_interop_MatrixSource_Kind_GPIO_EDGE)
        continue;
      if (!memcmp(s.node_mac, mac, 6) && !valid_input(s.gpio))
        return "GPIO input is not provisioned on this node";
      if (check_peers && memcmp(s.node_mac, mac, 6) &&
          !mesh_interop::instance().slave_node_exists(s.node_mac))
        return "GPIO source node is not paired";
      const auto owner = memcmp(s.node_mac, mac, 6) ? find_peer(s.node_mac) : -1;
      if (owner >= 0 && peers[owner].catalog_known && !(peers[owner].gpio_mask & (1ULL << s.gpio)))
        return "GPIO input is not provisioned on the source node";
    }
    for (size_t i = 0; i < c.conditions_count; ++i) {
      const auto& condition = c.conditions[i];
      if (condition.kind != mesh_interop_MatrixCondition_Kind_GPIO_STATE)
        continue;
      if (!memcmp(condition.node_mac, mac, 6) && !valid_input(condition.gpio))
        return "GPIO input is not provisioned on this node";
      if (check_peers && memcmp(condition.node_mac, mac, 6) &&
          !mesh_interop::instance().slave_node_exists(condition.node_mac))
        return "GPIO condition node is not paired";
      const auto owner = memcmp(condition.node_mac, mac, 6) ? find_peer(condition.node_mac) : -1;
      if (owner >= 0 && peers[owner].catalog_known &&
          !(peers[owner].gpio_mask & (1ULL << condition.gpio)))
        return "GPIO input is not provisioned on the condition node";
    }
    for (size_t i = 0; i < c.signals_count; ++i) {
      const auto& signal = c.signals[i];
      if (signal.kind == mesh_interop_SignalGenerator_Kind_SENSOR && check_peers &&
          memcmp(signal.node_mac, mac, 6) &&
          !mesh_interop::instance().slave_node_exists(signal.node_mac))
        return "Sensor node is not paired";
    }
    return nullptr;
  }
  static void gpio_isr(void* arg) {
    const auto input = uintptr_t(arg);
    gpio_event e{uint64_t(esp_timer_get_time()), uint32_t(input & 0x3f), false};
    e.rising = bool(gpio_get_level(static_cast<gpio_num_t>(e.pin))) != bool(input & 0x40);
    BaseType_t wake = pdFALSE;
    if (xQueueSendFromISR(gpio_queue, &e, &wake) != pdTRUE)
      gpio_overflow.store(true);
    if (worker_task)
      vTaskNotifyGiveFromISR(worker_task, &wake);
    if (wake)
      portYIELD_FROM_ISR();
  }
  static bool valid_input(uint32_t pin) {
    uint8_t pull = 0;
    return pin <= 39 && connected_lighting_drivers::gpio_input(pin, pull) &&
           GPIO_IS_VALID_GPIO(static_cast<int>(pin)) && pin != CONFIG_DISCOVERY_GPIO &&
           !connected_lighting_drivers::uses_gpio(pin) && !(pin >= 6 && pin <= 11);
  }
  static uint64_t input_mask() {
    uint64_t mask = 0;
    for (uint32_t pin = 0; pin < 40; ++pin)
      if (valid_input(pin))
        mask |= 1ULL << pin;
    return mask;
  }
  uint64_t initialized_input_mask() const {
    uint64_t mask = 0;
    for (uint32_t pin = 0; pin < 40; ++pin)
      if (configured_pins[pin])
        mask |= 1ULL << pin;
    return mask;
  }
  uint64_t read_input_levels(uint64_t mask) {
    uint64_t levels = 0;
    for (uint32_t pin = 0; pin < 40; ++pin)
      if (mask & (1ULL << pin)) {
        uint8_t pull = 0;
        bool inverted = false;
        if (connected_lighting_drivers::gpio_input(pin, pull, &inverted) &&
            (bool(gpio_get_level(static_cast<gpio_num_t>(pin))) != inverted))
          levels |= 1ULL << pin;
      }
    return levels;
  }
  static bool lookup_gpio(void* arg, const uint8_t* owner, uint32_t pin, bool& level) {
    auto& s = *static_cast<service*>(arg);
    if (pin > 39)
      return false;
    const uint64_t bit = 1ULL << pin;
    if (!memcmp(owner, s.mac, 6)) {
      if (!(s.local_gpio_known & bit))
        return false;
      level = (s.local_gpio_high & bit) != 0;
      return true;
    }
    for (size_t i = 0; i < s.peer_count; ++i)
      if (!memcmp(owner, s.peers[i].mac, 6)) {
        const auto& p = s.peers[i];
        if (!(p.gpio_known & bit))
          return false;
        level = (p.gpio_high & bit) != 0;
        return true;
      }
    return false;
  }
  template <typename definitions_type> bool prepare_gpio(const definitions_type& d) {
    if (d.gpio_inputs_count > 32)
      return false;
    for (size_t i = 0; i < d.gpio_inputs_count; ++i)
      if (!valid_input(d.gpio_inputs[i]))
        return false;
    auto err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
      return false;
    std::array<bool, 40> added{};
    for (size_t i = 0; i < d.gpio_inputs_count; ++i) {
      const auto pin = d.gpio_inputs[i];
      if (configured_pins[pin])
        continue;
      gpio_config_t cfg{};
      cfg.pin_bit_mask = 1ULL << pin;
      cfg.mode = GPIO_MODE_INPUT;
      cfg.intr_type = GPIO_INTR_ANYEDGE;
      uint8_t pull = hardware_config::PULL_NONE;
      bool inverted = false;
      if (!connected_lighting_drivers::gpio_input(pin, pull, &inverted))
        return false;
      cfg.pull_up_en = pull == hardware_config::PULL_UP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
      cfg.pull_down_en =
          pull == hardware_config::PULL_DOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
      if (gpio_config(&cfg) != ESP_OK ||
          gpio_isr_handler_add(static_cast<gpio_num_t>(pin), gpio_isr,
                               reinterpret_cast<void*>(uintptr_t(pin) | (inverted ? 0x40 : 0))) !=
              ESP_OK) {
        for (uint32_t j = 0; j < 40; ++j)
          if (added[j]) {
            gpio_isr_handler_remove(static_cast<gpio_num_t>(j));
            configured_pins[j] = false;
          }
        return false;
      }
      configured_pins[pin] = true;
      added[pin] = true;
    }
    return true;
  }
  bool prepare_gpio(const matrix_config& c) {
    return prepare_gpio(playback_definitions(c, mac, session, revision));
  }
  bool save(const std::vector<uint8_t>& bytes, uint64_t rev) {
    std::vector<uint8_t> record;
    w::put(record, w::MAGIC, 4);
    w::put(record, RECORD_VERSION, 4);
    w::put(record, rev, 8);
    w::put(record, crc32(bytes.data(), bytes.size()), 4);
    w::put(record, bytes.size(), 4);
    record.insert(record.end(), bytes.begin(), bytes.end());
    nvs_handle_t handle;
    if (nvs_open("matrix", NVS_READWRITE, &handle) != ESP_OK)
      return false;
    auto err = nvs_set_blob(handle, "config", record.data(), record.size());
    if (err == ESP_OK)
      err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
  }
  template <typename config_type> bool prepare_runtime(const config_type& c, uint64_t rev) {
    auto next_ledger = make_heap_object<player>();
    if (!next_ledger) {
      ESP_LOGW("matrix", "Unable to allocate replacement ledger");
      return false;
    }
    if (!next_ledger->prepare(c, nullptr, session, rev)) {
      ESP_LOGW("matrix", "Unable to prepare replacement ledger");
      return false;
    }
    auto next_local = make_heap_object<player>(connected_lighting_drivers::find);
    if (!next_local) {
      ESP_LOGW("matrix", "Unable to allocate replacement local player");
      return false;
    }
    if (!next_local->prepare(c, mac, session, rev)) {
      ESP_LOGW("matrix", "Unable to prepare replacement local player");
      return false;
    }
    prepared_ledger = std::move(next_ledger);
    prepared_local = std::move(next_local);
    return true;
  }
  bool load() {
    nvs_handle_t handle;
    const auto opened = nvs_open("matrix", NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND)
      return false;
    if (opened != ESP_OK) {
      error("Unable to read saved matrix", true);
      return true;
    }
    size_t size = 0;
    auto err = nvs_get_blob(handle, "config", nullptr, &size);
    std::vector<uint8_t> record;
    if (err == ESP_OK && size >= 24 && size <= MAX_BYTES + 24) {
      record.resize(size);
      err = nvs_get_blob(handle, "config", record.data(), &size);
    }
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
      return false;
    if (record.empty() || err != ESP_OK || w::get(record.data(), 4) != w::MAGIC ||
        w::get(record.data() + 20, 4) != size - 24 ||
        crc32(record.data() + 24, size - 24) != w::get(record.data() + 16, 4)) {
      error("Saved matrix record is invalid", true);
      return true;
    }
    if (w::get(record.data() + 4, 4) != RECORD_VERSION) {
      ESP_LOGW("matrix", "Discarding obsolete development matrix record");
      return false;
    }
    auto c = make_heap_object<runtime_config>();
    if (!c || !decode_compact_config(record.data() + 24, size - 24, *c) ||
        platform_validate(*c, false) ||
        !prepare_gpio(playback_definitions(*c, mac, session, revision))) {
      error("Saved matrix cannot be restored", true);
      return true;
    }
    revision = w::get(record.data() + 8, 8);
    std::vector<uint8_t>().swap(record);
    if (!c->reencode()) {
      error("Unable to encode saved matrix", true);
      return true;
    }
    if (!prepare_runtime(*c, revision)) {
      error("Unable to prepare saved matrix", true);
      return true;
    }
    configuration_persisted = true;
    start_deployment(std::move(c));
    return true;
  }
  // Keeping these worker stages out of run() avoids reserving every stage's
  // temporary stack space while nested BLE preview requests are processed.
  [[gnu::noinline]] void initialize_configuration() {
    if (connected_lighting_drivers::status() != ESP_OK) {
      error("Hardware configuration missing or invalid; lighting disabled", true);
      return;
    }
    // The master owns configuration and startup animation timing. Slaves wait
    // for its synchronized snapshot rather than displaying an unrelated default.
    if (!master)
      return;
    if (load())
      return;
    auto c = make_default_config(mac, connected_lighting_drivers::find);
    if (!c || validate(*c, mac) || !prepare_runtime(*c, 1)) {
      error("Unable to prepare default matrix", true);
      return;
    }
    configuration_persisted = !c->encoded().empty() && save(c->encoded(), 1);
    if (!configuration_persisted)
      error("Default matrix is active in RAM; NVS save failed");
    revision = 1;
    start_deployment(std::move(c));
  }
  void extend_default_layout() {
    if (!default_layout_dirty || !config || !config->factory_default || deployment ||
        pending_snapshot || ble_rx.id())
      return;
    default_layout_dirty = false;
    auto next = make_heap_object<runtime_config>(*config);
    if (!next)
      return;
    bool changed = false;
    for (size_t i = 0; i < peer_count; ++i) {
      const auto& peer = peers[i];
      if (!peer.catalog_known || !mesh_interop::instance().slave_node_exists(peer.mac))
        continue;
      for (size_t j = 0; j < peer.outputs_count; ++j)
        changed |= add_default_output(*next, peer.mac, peer.outputs[j].driver,
                                      peer.outputs[j].pixel_count, peer.outputs[j].source_index);
    }
    if (!changed || revision == UINT64_MAX || platform_validate(*next, true))
      return;
    if (!next->reencode() || !prepare_runtime(*next, revision + 1))
      return;
    if (!save(next->encoded(), revision + 1)) {
      prepared_ledger.reset();
      prepared_local.reset();
      error("Unable to save default layout");
      return;
    }
    configuration_persisted = true;
    ++revision;
    start_deployment(std::move(next));
  }
  void start_deployment(heap_object<runtime_config> compact) {
    preview_player.reset();
    preview_distributing = false;
    preview_bytes.clear();
    candidate = std::move(compact);
    deployment = true;
    state.stage = STAGE_DISTRIBUTING;
    state.revision = revision;
    tx = outbound{};
    for (auto& p : peers) {
      p.attempted = false;
      p.replacement_prepared = false;
    }
  }
  size_t own_outputs(mesh_interop_LayoutOutput* outputs) {
    size_t count = 0;
    for (uint32_t id = 1; id <= MAX_DRIVERS_PER_NODE; ++id) {
      auto* driver = connected_lighting_drivers::find(id);
      if (!driver)
        continue;
      for (uint32_t source = 0;
           source < driver->get_layout_source_count() && count < MAX_SOURCES_PER_NODE; ++source) {
        auto& o = outputs[count++];
        o = {};
        memcpy(o.node_mac, mac, 6);
        o.driver = static_cast<mesh_interop_MatrixDriver>(id);
        o.pixel_count = driver->get_layout_default_pixel_count(source);
        o.online = true;
        o.ready = driver->_is_ready();
        o.known = true;
        o.source_index = source;
        o.max_pixel_count = driver->get_layout_max_pixel_count(source);
        o.driver_kind =
            static_cast<mesh_interop_MatrixDriverKind>(driver->get_layout_source_kind(source));
      }
    }
    return count;
  }
  void send_inventory(const uint8_t* destination) {
    size_t count = 0;
    for (uint32_t id = 1; id <= MAX_DRIVERS_PER_NODE; ++id) {
      const auto* driver = connected_lighting_drivers::find(id);
      if (driver)
        count += driver->get_layout_source_count();
    }
    if (count > MAX_SOURCES_PER_NODE) {
      ESP_LOGE("matrix", "Too many local lighting outputs for inventory");
      return;
    }
    std::vector<uint8_t> body;
    w::put(body, session, 8);
    w::put(body, count, 1);
    for (uint32_t id = 1; id <= MAX_DRIVERS_PER_NODE; ++id) {
      auto* driver = connected_lighting_drivers::find(id);
      if (!driver)
        continue;
      for (size_t source = 0; source < driver->get_layout_source_count(); ++source) {
        w::put(body, id, 1);
        w::put(body, source, 1);
        w::put(body, static_cast<uint8_t>(driver->get_layout_source_kind(source)), 1);
        w::put(body, driver->_is_ready(), 1);
        w::put(body, driver->get_layout_default_pixel_count(source), 4);
        w::put(body, driver->get_layout_max_pixel_count(source), 4);
      }
    }
    w::put(body, input_mask(), 8);
    const auto* version = esp_app_get_description()->version;
    const auto length = strnlen(version, 31);
    w::put(body, length, 1);
    body.insert(body.end(), version, version + length);
    send(destination, w::INVENTORY, body);
  }
  void publish_layout(mesh_interop_LayoutResponse& response) {
    auto bytes = encode(mesh_interop_LayoutResponse_fields, &response);
    std::lock_guard<std::mutex> lock(status_mutex);
    if (bytes.size() > layout_response_bytes.size())
      return;
    memcpy(layout_response_bytes.data(), bytes.data(), bytes.size());
    layout_response_size = bytes.size();
  }
  void layout_error(uint32_t request_id, const char* message) {
    mesh_interop_LayoutResponse response{};
    response.request_id = request_id;
    snprintf(response.error, sizeof(response.error), "%s", message);
    publish_layout(response);
  }
  bool capture_layout(const packet& packet, uint64_t now) {
    snapshot_is_matrix = false;
    snapshot_is_active = false;
    auto layout_snapshot = make_heap_object<mesh_interop_LayoutSnapshot>();
    if (!layout_snapshot)
      return false;
    auto& snapshot = *layout_snapshot;
    snapshot.master_session = session;
    snapshot.revision = revision;
    memcpy(snapshot.master_mac, mac, 6);
    const auto* saved = candidate ? candidate.get() : config.get();
    if (saved) {
      snapshot.layout_count = saved->layout_count;
      snapshot.has_center_index = saved->has_center_index;
      snapshot.center_index = saved->center_index;
      std::copy_n(saved->layout.data(), saved->layout_count, snapshot.layout);
    }
    snapshot.outputs_count = own_outputs(snapshot.outputs);
    auto mesh = mesh_interop::instance().get_state();
    for (size_t i = 0; i < mesh.nodes_count; ++i)
      find_peer(mesh.nodes[i].mac);
    for (size_t i = 0; i < peer_count; ++i) {
      const auto& p = peers[i];
      if (!p.catalog_known)
        ++snapshot.pending_nodes;
      if (p.catalog_known) {
        for (size_t j = 0; j < p.outputs_count && snapshot.outputs_count < MAX_INVENTORY_SOURCES;
             ++j) {
          auto& out = snapshot.outputs[snapshot.outputs_count++];
          out = p.outputs[j];
          out.online = available(p, now);
        }
      } else if (saved) {
        // Retain configured, currently unknown outputs without inventing hardware lengths.
        for (size_t j = 0;
             j < saved->layout_count && snapshot.outputs_count < MAX_INVENTORY_SOURCES; ++j) {
          if (memcmp(saved->layout[j].node_mac, p.mac, 6))
            continue;
          auto& out = snapshot.outputs[snapshot.outputs_count++];
          memcpy(out.node_mac, p.mac, 6);
          out.driver = saved->layout[j].driver;
          out.online = available(p, now);
          out.source_index = saved->layout[j].source_index;
        }
      }
    }
    layout_snapshot_bytes = encode(mesh_interop_LayoutSnapshot_fields, &snapshot, 32768);
    snapshot_outputs.assign(snapshot.outputs, snapshot.outputs + snapshot.outputs_count);
    layout_snapshot_id = esp_random();
    if (!layout_snapshot_id)
      layout_snapshot_id = 1;
    layout_generation = packet.generation;
    layout_connection = packet.connection;
    layout_touched = now;
    return !layout_snapshot_bytes.empty();
  }
  bool capture_matrix(const packet& packet, uint64_t now, bool active = false) {
    const auto* saved =
        active      ? (local ? (retiring_config ? retiring_config.get() : config.get()) : nullptr)
        : candidate ? candidate.get()
                    : config.get();
    const std::vector<uint8_t> empty_config{8, SCHEMA_VERSION};
    const auto& bytes = saved ? saved->encoded() : empty_config;
    if (bytes.empty())
      return false;
    auto mesh = mesh_interop::instance().get_state();
    for (size_t i = 0; i < mesh.nodes_count; ++i)
      find_peer(mesh.nodes[i].mac);
    const uint64_t active_revision = local ? local->definitions().revision : 0;
    const auto* version = esp_app_get_description()->version;
    const auto write = [&](pb_ostream_t& stream) {
      const auto scalar = [&](uint32_t tag, uint64_t value) {
        return !value ||
               (pb_encode_tag(&stream, PB_WT_VARINT, tag) && pb_encode_varint(&stream, value));
      };
      const auto data = [&](uint32_t tag, const uint8_t* value, size_t length) {
        return pb_encode_tag(&stream, PB_WT_STRING, tag) &&
               pb_encode_string(&stream, value, length);
      };
      const auto add = [&](const uint8_t* address, uint64_t mask, bool known, bool online,
                           const char* node_version) {
        mesh_interop_MatrixInputNode node{};
        memcpy(node.node_mac, address, 6);
        node.known = known;
        node.online = online;
        snprintf(node.firmware_version, sizeof(node.firmware_version), "%.*s", 31, node_version);
        for (uint32_t pin = 0; pin < 40; ++pin)
          if (mask & (1ULL << pin))
            node.gpios[node.gpios_count++] = pin;
        return pb_encode_tag(&stream, PB_WT_STRING, mesh_interop_MatrixDocument_nodes_tag) &&
               pb_encode_submessage(&stream, mesh_interop_MatrixInputNode_fields, &node);
      };
      if (!data(mesh_interop_MatrixDocument_config_tag, bytes.data(), bytes.size()) ||
          !scalar(mesh_interop_MatrixDocument_revision_tag, active ? active_revision : revision) ||
          !scalar(mesh_interop_MatrixDocument_master_session_tag, session) ||
          !data(mesh_interop_MatrixDocument_master_mac_tag, mac, 6) ||
          !add(mac, input_mask(), true, true, version))
        return false;
      size_t count = 1;
      for (size_t i = 0; i < peer_count && count < 17; ++i)
        if (mesh_interop::instance().slave_node_exists(peers[i].mac)) {
          if (!add(peers[i].mac, peers[i].gpio_mask, peers[i].catalog_known,
                   available(peers[i], now), peers[i].firmware_version))
            return false;
          ++count;
        }
      return scalar(mesh_interop_MatrixDocument_active_revision_tag, active_revision) &&
             scalar(mesh_interop_MatrixDocument_activation_pending_tag,
                    deployment || pending_snapshot != nullptr) &&
             scalar(mesh_interop_MatrixDocument_persisted_tag,
                    active ? active_persisted : configuration_persisted) &&
             data(mesh_interop_MatrixDocument_master_firmware_version_tag,
                  reinterpret_cast<const uint8_t*>(version), strnlen(version, 31));
    };
    pb_ostream_t size = PB_OSTREAM_SIZING;
    if (!write(size) || size.bytes_written > 32768)
      return false;
    std::vector<uint8_t> result(size.bytes_written);
    auto stream = pb_ostream_from_buffer(result.data(), result.size());
    if (!write(stream))
      return false;
    layout_snapshot_bytes = std::move(result);
    snapshot_outputs.clear();
    snapshot_is_matrix = true;
    snapshot_is_active = active;
    layout_snapshot_id = esp_random();
    if (!layout_snapshot_id)
      layout_snapshot_id = 1;
    layout_generation = packet.generation;
    layout_connection = packet.connection;
    layout_touched = now;
    return !layout_snapshot_bytes.empty();
  }
  void clear_identified_source() {
    if (identify_driver == IDENTIFY_ALL_DRIVERS) {
      for (uint32_t id = 1; id <= connected_lighting_drivers::MAX_DRIVERS; ++id) {
        auto* driver = connected_lighting_drivers::find(id);
        if (driver && driver->_is_ready()) {
          driver->clear();
          driver->show();
        }
      }
      return;
    }
    auto* driver = connected_lighting_drivers::find(identify_driver);
    if (!driver)
      return;
    const auto length = driver->get_layout_pixel_count(identify_source);
    linear_lighting_driver_view view(*driver, 0, length, length, identify_source);
    view.clear();
    view.show();
  }
  bool begin_identify(uint32_t driver_id, uint64_t now, uint32_t source = 0, uint32_t length = 0,
                      bool reversed = false) {
    if (driver_id == IDENTIFY_ALL_DRIVERS) {
      if (source || length || reversed)
        return false;
      bool found = false;
      for (uint32_t id = 1; id <= connected_lighting_drivers::MAX_DRIVERS; ++id) {
        auto* driver = connected_lighting_drivers::find(id);
        found |= driver && driver->_is_ready() && driver->get_layout_source_count();
      }
      if (!found)
        return false;
      if (identify_driver)
        clear_identified_source();
      identify_driver = driver_id;
      identify_source = 0;
      identify_length = 0;
      identify_reversed = false;
      identify_until = now + 3000000;
      return true;
    }
    auto* driver = connected_lighting_drivers::find(driver_id);
    if (!driver || !driver->_is_ready() || source >= driver->get_layout_source_count() ||
        !driver->get_layout_pixel_count(source))
      return false;
    const auto maximum = driver->get_layout_max_pixel_count(source);
    if (length > maximum || (length && driver->reserve_layout_pixels(source, length) != ESP_OK))
      return false;
    const auto capacity = driver->get_layout_pixel_count(source);
    if (length > capacity)
      return false;
    if (identify_driver && (identify_driver != driver_id || identify_source != source))
      clear_identified_source();
    identify_driver = driver_id;
    identify_source = source;
    identify_length = length ? length : capacity;
    identify_reversed = reversed;
    identify_until = now + 3000000;
    return true;
  }
  bool prepare_preview(const mesh_interop_AnimationPreview& message) {
    if (!local || !message.has_effect || message.master_session != session ||
        message.revision != local->definitions().revision ||
        message.end_at_us <= message.start_at_us ||
        message.end_at_us - message.start_at_us > 10000000 ||
        !local->definitions().output_mappings_count)
      return false;
    auto player = make_heap_object<matrix::player>(connected_lighting_drivers::find);
    if (!player || !player->prepare_preview(local->definitions(), message.effect))
      return false;
    animation_command command{};
    command.master_session = session;
    command.revision = message.revision;
    command.entry_id = 1;
    command.effective_at_us = message.start_at_us;
    if (!player->enqueue(command))
      return false;
    preview_start = message.start_at_us;
    preview_end = message.end_at_us;
    preview_player = std::move(player);
    return true;
  }
  bool begin_animation_preview(uint32_t effect_id, uint64_t clock) {
    if (!config || !local || deployment || pending_snapshot || tx.peer_index >= 0 || !effect_id)
      return false;
    const auto it = std::find_if(config->effects.begin(), config->effects.end(),
                                 [&](const auto& effect) { return effect.id == effect_id; });
    if (it == config->effects.end())
      return false;
    bool has_output = local->definitions().output_mappings_count != 0;
    for (size_t i = 0; i < peer_count; ++i)
      if (peers[i].ready && available(peers[i], esp_timer_get_time())) {
        has_output |=
            std::any_of(config->layout.begin(), config->layout.end(),
                        [&](const auto& row) { return !memcmp(row.node_mac, peers[i].mac, 6); });
      }
    if (!has_output)
      return false;
    auto message = make_heap_object<mesh_interop_AnimationPreview>();
    if (!message)
      return false;
    message->master_session = session;
    message->revision = revision;
    message->start_at_us = clock + 500000 + peer_count * 500000;
    const uint64_t duration = !it->has_modulation && it->duration_ms
                                  ? std::min<uint64_t>(it->duration_ms * 1000ULL, 10000000)
                                  : 10000000;
    message->end_at_us = message->start_at_us + duration;
    message->has_effect = true;
    message->effect = *it;
    auto bytes = encode(mesh_interop_AnimationPreview_fields, message.get());
    if (bytes.empty())
      return false;
    if (local->definitions().output_mappings_count && !prepare_preview(*message))
      return false;
    preview_start = message->start_at_us;
    preview_end = message->end_at_us;
    preview_bytes = std::move(bytes);
    preview_distributing = true;
    for (auto& peer : peers)
      peer.preview_attempted = false;
    return true;
  }
  void handle_layout_request(const packet& packet, const mesh_interop_LayoutRequest& request,
                             uint64_t now) {
    if (!request.request_id)
      return;
    std::underlying_type_t<mesh_interop_LayoutRequest_Operation> operation;
    memcpy(&operation, &request.operation, sizeof(operation));
    if (operation == mesh_interop_LayoutRequest_Operation_PREVIEW) {
      if (!request.has_identify_options ||
          !begin_animation_preview(request.identify_options,
                                   uint64_t(master_clock_manager::instance().get_master_time()))) {
        layout_error(request.request_id, "Unable to start animation preview");
        return;
      }
      mesh_interop_LayoutResponse response{};
      response.request_id = request.request_id;
      response.complete = true;
      publish_layout(response);
      return;
    }
    const bool active_read = operation == mesh_interop_LayoutRequest_Operation_READ_ACTIVE_MATRIX;
    const bool matrix_read =
        operation == mesh_interop_LayoutRequest_Operation_READ_MATRIX || active_read;
    const bool reading = matrix_read || operation == mesh_interop_LayoutRequest_Operation_READ;
    if ((!reading && operation != mesh_interop_LayoutRequest_Operation_IDENTIFY) ||
        (reading && !request.snapshot_id && request.offset)) {
      layout_error(request.request_id, "Invalid layout request");
      return;
    }
    if (identifying.active) {
      layout_error(request.request_id, "Identification is already in progress");
      return;
    }
    if (reading && !request.snapshot_id && !request.offset) {
      if (!(matrix_read ? capture_matrix(packet, now, active_read) : capture_layout(packet, now))) {
        layout_error(request.request_id, "Unable to read configuration");
        return;
      }
    }
    if (layout_snapshot_bytes.empty() ||
        (reading && (matrix_read != snapshot_is_matrix ||
                     (matrix_read && active_read != snapshot_is_active))) ||
        (request.snapshot_id && request.snapshot_id != layout_snapshot_id) ||
        packet.generation != layout_generation || packet.connection != layout_connection ||
        now - layout_touched >= TRANSFER_TIMEOUT_US) {
      layout_error(request.request_id, "Layout snapshot expired; refresh and try again");
      return;
    }
    layout_touched = now;
    if (operation == mesh_interop_LayoutRequest_Operation_IDENTIFY) {
      const uint32_t output_index =
          request.has_identify_options ? request.identify_options & 0xffu : request.output_index;
      const uint32_t length =
          request.has_identify_options ? (request.identify_options >> 8) & 0xfffu : 0;
      const bool reversed = request.has_identify_options && (request.identify_options & (1u << 20));
      const bool all = request.has_identify_options && (request.identify_options & (1u << 21));
      if (request.has_identify_options &&
          (request.identify_options >> 22 || (all && (length || reversed)))) {
        layout_error(request.request_id, "Invalid identify options");
        return;
      }
      if (snapshot_is_matrix || request.snapshot_id != layout_snapshot_id ||
          output_index >= snapshot_outputs.size()) {
        layout_error(request.request_id, "Unknown output");
        return;
      }
      const auto& output = snapshot_outputs[output_index];
      if (!output.known || length > output.max_pixel_count) {
        layout_error(request.request_id, "Identify length exceeds output capacity");
        return;
      }
      if (!memcmp(output.node_mac, mac, 6)) {
        if (!begin_identify(all ? IDENTIFY_ALL_DRIVERS : uint32_t(output.driver), now,
                            all ? 0 : output.source_index, length, reversed)) {
          layout_error(request.request_id, "Output is unavailable");
          return;
        }
        mesh_interop_LayoutResponse response{};
        response.request_id = request.request_id;
        response.complete = true;
        publish_layout(response);
        return;
      }
      const auto index = find_peer(output.node_mac);
      if (index < 0 || !available(peers[index], now) || !output.ready || !output.known) {
        layout_error(request.request_id, "Output is offline or unavailable");
        return;
      }
      identifying = {};
      identifying.active = true;
      identifying.peer_index = index;
      identifying.request_id = request.request_id;
      identifying.driver = all ? IDENTIFY_ALL_DRIVERS : uint32_t(output.driver);
      identifying.source = all ? 0 : output.source_index;
      identifying.length = length;
      identifying.reversed = reversed;
      identifying.wire_id = esp_random();
      if (!identifying.wire_id)
        identifying.wire_id = 1;
      identifying.generation = packet.generation;
      return;
    }
    if (!reading || request.offset > layout_snapshot_bytes.size()) {
      layout_error(request.request_id, "Invalid layout read");
      return;
    }
    mesh_interop_LayoutResponse response{};
    response.request_id = request.request_id;
    response.snapshot_id = layout_snapshot_id;
    response.total_size = layout_snapshot_bytes.size();
    response.offset = request.offset;
    response.data.size = std::min<size_t>(sizeof(response.data.bytes),
                                          layout_snapshot_bytes.size() - request.offset);
    memcpy(response.data.bytes, layout_snapshot_bytes.data() + request.offset, response.data.size);
    response.complete = request.offset + response.data.size == layout_snapshot_bytes.size();
    publish_layout(response);
  }
  void handle_upload(const packet& p, uint64_t now) {
    if (!master || p.generation != ble_generation.load())
      return;
    mesh_interop_MatrixUpload u{};
    if (!decode(mesh_interop_MatrixUpload_fields, &u, p.data, p.size)) {
      error("Malformed upload envelope", true);
      return;
    }
    if (u.which_operation == mesh_interop_MatrixUpload_layout_request_tag) {
      handle_layout_request(p, u.operation.layout_request, now);
      return;
    }
    if (!ble_rx.id() && u.which_operation == mesh_interop_MatrixUpload_abort_tag &&
        u.operation.abort == state.transfer_id)
      return;
    if (!ble_rx.id() && u.which_operation == mesh_interop_MatrixUpload_commit_tag &&
        u.operation.commit == state.transfer_id &&
        (deployment || pending_snapshot || state.stage == STAGE_ACTIVE))
      return;
    if (u.which_operation == mesh_interop_MatrixUpload_begin_tag ||
        u.which_operation == mesh_interop_MatrixUpload_layout_begin_tag ||
        u.which_operation == mesh_interop_MatrixUpload_matrix_begin_tag) {
      if (deployment || pending_snapshot || preview_distributing ||
          (tx.peer_index >= 0 && tx.kind == 3)) {
        error("Another deployment is in progress", true);
        return;
      }
      if (ble_rx.id() && (upload_connection != p.connection || upload_generation != p.generation)) {
        error("Upload is owned by another connection", true);
        return;
      }
      const bool layout_upload = u.which_operation == mesh_interop_MatrixUpload_layout_begin_tag;
      const bool matrix_upload = u.which_operation == mesh_interop_MatrixUpload_matrix_begin_tag;
      if (ble_rx.id() && (upload_is_layout != layout_upload || upload_is_matrix != matrix_upload)) {
        error("Conflicting upload type", true);
        return;
      }
      const auto& b = layout_upload   ? u.operation.layout_begin
                      : matrix_upload ? u.operation.matrix_begin
                                      : u.operation.begin;
      if (!ble_rx.begin(b.transfer_id, b.total_size, b.crc32, now)) {
        error("Invalid transfer size or conflicting transfer metadata", true);
        return;
      }
      upload_connection = p.connection;
      upload_generation = p.generation;
      upload_is_layout = layout_upload;
      upload_is_matrix = matrix_upload;
      state.transfer_id = b.transfer_id;
      state.received_bytes = ble_rx.offset();
      state.stage = STAGE_RECEIVING;
      state.error[0] = 0;
      return;
    }
    if (p.connection != upload_connection || p.generation != upload_generation || !ble_rx.id()) {
      error("No matching upload session", true);
      return;
    }
    if (u.which_operation == mesh_interop_MatrixUpload_chunk_tag) {
      auto& c = u.operation.chunk;
      if (!ble_rx.append(c.transfer_id, c.offset, c.data.bytes, c.data.size, now))
        error("Invalid upload offset or chunk", true);
      else
        state.received_bytes = ble_rx.offset();
    } else if (u.which_operation == mesh_interop_MatrixUpload_abort_tag) {
      if (u.operation.abort == ble_rx.id()) {
        ble_rx.reset();
        state.stage = config ? STAGE_ACTIVE : mesh_interop_MatrixStatus_Stage_IDLE;
      }
    } else if (u.which_operation == mesh_interop_MatrixUpload_commit_tag) {
      if (!ble_rx.complete(u.operation.commit)) {
        error("Upload is incomplete or CRC32 failed", true);
        return;
      }
      state.stage = mesh_interop_MatrixStatus_Stage_VALIDATING;
      heap_object<runtime_config> compact;
      if (upload_is_layout || upload_is_matrix) {
        compact =
            config ? make_heap_object<runtime_config>(*config) : make_heap_object<runtime_config>();
        if (!compact) {
          error("Unable to allocate matrix", true);
          ble_rx.reset();
          return;
        }
        uint64_t expected_revision, master_session;
        if (!decode_compact_editor_update(ble_rx.bytes(), upload_is_layout, *compact,
                                          expected_revision, master_session)) {
          error(upload_is_layout ? "Invalid layout protobuf" : "Invalid matrix update protobuf",
                true);
          ble_rx.reset();
          return;
        }
        if (expected_revision != revision || master_session != session) {
          error("Configuration changed on controller; reload before applying", true);
          ble_rx.reset();
          return;
        }
      } else {
        compact = make_heap_object<runtime_config>();
        if (!compact) {
          error("Unable to allocate matrix", true);
          ble_rx.reset();
          return;
        }
        if (!decode_compact_config(ble_rx.bytes(), *compact)) {
          error("Invalid matrix protobuf", true);
          ble_rx.reset();
          return;
        }
      }
      compact->factory_default = false;
      if (auto e = platform_validate(*compact, true)) {
        error(e, true);
        ble_rx.reset();
        return;
      }
      if (!compact->reencode()) {
        error("Configuration exceeds 16 KiB", true);
        ble_rx.reset();
        return;
      }
      ble_rx.reset();
      if (revision == UINT64_MAX || !prepare_runtime(*compact, revision + 1)) {
        log_memory();
        error("Unable to prepare replacement runtime", true);
        ble_rx.reset();
        return;
      }
      if (!prepare_gpio(playback_definitions(*compact, mac, session, revision + 1))) {
        prepared_ledger.reset();
        prepared_local.reset();
        error("Unable to configure matrix GPIOs", true);
        return;
      }
      if (revision == UINT64_MAX || !save(compact->encoded(), revision + 1)) {
        prepared_ledger.reset();
        prepared_local.reset();
        cleanup_gpio();
        error("Unable to persist matrix", true);
        ble_rx.reset();
        return;
      }
      configuration_persisted = true;
      ++revision;
      start_deployment(std::move(compact));
    } else
      error("Unknown upload operation", true);
  }
  void begin_transfer(int index, uint8_t kind, std::vector<uint8_t> bytes) {
    tx = outbound{};
    tx.peer_index = index;
    tx.kind = kind;
    tx.bytes = std::move(bytes);
    tx.id = esp_random();
    if (!tx.id)
      tx.id = 1;
    tx.checksum = crc32(tx.bytes.data(), tx.bytes.size());
    peers[index].snapshotting = kind == 2;
    peers[index].last_recover = esp_timer_get_time();
  }
  [[gnu::noinline]] void transfer_step(uint64_t now) {
    if (tx.peer_index < 0 || (tx.last_send && now - tx.last_send < 100000))
      return;
    auto& p = peers[tx.peer_index];
    if (tx.retries++ >= 4 || tx.bytes.empty()) {
      if (tx.kind == 3) {
        p.preview_attempted = true;
        tx = outbound{};
        error("Node preview transfer timed out");
        return;
      }
      p.ready = false;
      p.snapshotting = false;
      p.attempted = true;
      p.commands.clear();
      error("Node transfer timed out");
      tx = outbound{};
      return;
    }
    std::vector<uint8_t> b;
    w::put(b, tx.id, 4);
    if (tx.phase == 0) {
      w::put(b, tx.kind, 1);
      w::put(b, tx.bytes.size(), 4);
      w::put(b, tx.checksum, 4);
      w::put(b, revision, 8);
      w::put(b, session, 8);
      send(p.mac, w::BEGIN, b);
    } else if (tx.phase == 1) {
      w::put(b, tx.offset, 4);
      tx.sent_end = std::min(tx.offset + w::CHUNK_SIZE, tx.bytes.size());
      b.insert(b.end(), tx.bytes.begin() + tx.offset, tx.bytes.begin() + tx.sent_end);
      send(p.mac, w::CHUNK, b);
    } else
      send(p.mac, w::FINISH, b);
    tx.last_send = now;
  }
  void transfer_ack(int index, const uint8_t* b, size_t n) {
    if (n != 10 || tx.peer_index != index || w::get(b, 4) != tx.id || b[8] != tx.phase)
      return;
    auto& p = peers[index];
    if (b[9] != 0) {
      if (tx.kind == 3) {
        p.preview_attempted = true;
        tx = outbound{};
        error("Node rejected animation preview");
        return;
      }
      p.ready = false;
      p.snapshotting = false;
      p.attempted = true;
      // The slave may have rebooted before its new stream was observed. A
      // rejected snapshot can mean its RAM-only definitions are gone.
      if (tx.kind == 2)
        p.definitions = false;
      tx = outbound{};
      error("Slave rejected transferred data");
      return;
    }
    size_t offset = w::get(b + 4, 4);
    if (tx.phase == 0) {
      if (offset > tx.bytes.size())
        return;
      tx.offset = offset;
      tx.phase = offset == tx.bytes.size() ? 2 : 1;
    } else if (tx.phase == 1) {
      if (offset > tx.sent_end)
        return;
      tx.offset = offset;
      tx.phase = offset == tx.bytes.size() ? 2 : 1;
    } else {
      if (tx.kind == 3) {
        p.preview_attempted = true;
        tx = outbound{};
        return;
      }
      if (deployment && tx.kind == 1)
        p.replacement_prepared = true;
      else
        p.definitions = true;
      p.last_recover = 0;
      p.attempted = true;
      if (tx.kind == 2) {
        p.ready = true;
        p.snapshotting = false;
      }
      tx = outbound{};
      return;
    }
    tx.last_send = 0;
    tx.retries = 0;
  }
  void acknowledge(const uint8_t* dest, uint32_t id, uint8_t phase, bool ok) {
    std::vector<uint8_t> b;
    w::put(b, id, 4);
    w::put(b, radio_rx.offset(), 4);
    w::put(b, phase, 1);
    w::put(b, ok ? 0 : 1, 1);
    send(dest, w::ACK, b);
  }
  bool install_received() {
    if (inbound_kind == 3) {
      auto message = make_heap_object<mesh_interop_AnimationPreview>();
      if (!message ||
          !decode(mesh_interop_AnimationPreview_fields, message.get(), radio_rx.bytes().data(),
                  radio_rx.bytes().size()) ||
          message->revision != inbound_revision ||
          message->end_at_us <= uint64_t(master_clock_manager::instance().get_master_time()))
        return false;
      return prepare_preview(*message);
    }
    if (inbound_kind == 1) {
      auto d = make_heap_object<animation_definitions>();
      if (!d ||
          !decode(mesh_interop_AnimationDefinitions_fields, d.get(), radio_rx.bytes().data(),
                  radio_rx.bytes().size()) ||
          d->master_session != session || d->revision != inbound_revision)
        return false;
      if (local && d->revision < local->definitions().revision)
        return false;
      if (pending_player && d->revision < pending_player->definitions().revision)
        return false;
      auto p = make_heap_object<player>(connected_lighting_drivers::find);
      if (!p || !p->prepare(*d))
        return false;
      const auto* existing = pending_player ? pending_player.get() : local.get();
      if (existing && existing->definitions().revision == d->revision)
        return existing->definition_bytes() == radio_rx.bytes();
      if (!prepare_gpio(*d))
        return false;
      pending_player = std::move(p);
      pending_snapshot.reset();
      cleanup_gpio(true);
      return true;
    }
    auto s = make_heap_object<animation_snapshot>();
    if (!s)
      return false;
    if (!decode(mesh_interop_AnimationSnapshot_fields, s.get(), radio_rx.bytes().data(),
                radio_rx.bytes().size()) ||
        s->master_session != session || s->revision != inbound_revision)
      return false;
    if (local && s->revision == local->definitions().revision && s->sequence < local_sequence)
      return false;
    if (pending_snapshot && pending_player &&
        s->revision == pending_player->definitions().revision && s->sequence < local_sequence)
      return false;
    const auto* source = pending_player ? pending_player.get() : local.get();
    if (!source || source->definitions().revision != s->revision)
      return false;
    // Prepare alongside the active player: malformed recovery never changes it.
    auto p = make_heap_object<player>(connected_lighting_drivers::find);
    if (!p || !p->prepare(*source) || !p->restore(*s))
      return false;
    if (local && !pending_snapshot && s->revision != local->definitions().revision)
      previous_sequence = local_sequence;
    pending_player = std::move(p);
    local_sequence = s->sequence;
    pending_snapshot = std::move(s);
    return true;
  }
  bool accept_gpio_stream(peer& p, uint64_t stream) {
    if (!stream)
      return false;
    if (p.gpio_stream == stream)
      return true;
    for (auto retired : p.retired_gpio_streams)
      if (retired == stream)
        return false;
    if (p.gpio_stream) {
      // A new stream identifies a rebooted slave, which has lost all of its
      // RAM-only definitions even if it was already marked unavailable.
      const auto index = static_cast<int>(&p - peers.data());
      if (tx.peer_index == index)
        tx = outbound{};
      p.ready = false;
      p.definitions = false;
      p.snapshotting = false;
      p.replacement_prepared = false;
      p.attempted = false;
      p.commands.clear();
      p.retiring_commands.clear();
      p.last_retry = 0;
      p.retries = 0;
      p.last_recover = 0;
    }
    p.retired_gpio_streams[p.retired_gpio_index++ % p.retired_gpio_streams.size()] = p.gpio_stream;
    p.gpio_stream = stream;
    p.gpio_sequence = 0;
    p.gpio_degraded = false;
    p.gpio_known = p.gpio_high = p.gpio_state_at = p.gpio_snapshot_seen = 0;
    p.catalog_known = false;
    p.catalog_at = 0;
    p.catalog_retry = 0;
    p.firmware_version[0] = '\0';
    return true;
  }
  bool evaluate_gpio(const uint8_t* source_mac, const gpio_event& e, uint64_t rev, uint64_t clock) {
    const runtime_config* c = nullptr;
    evaluator* evaluation = nullptr;
    if (ledger && ledger->definitions().revision == rev &&
        (!retiring_config || e.at >= activate_at)) {
      c = config.get();
      evaluation = &rules;
    } else if (retiring_ledger && retiring_ledger->definitions().revision == rev &&
               e.at < activate_at) {
      c = retiring_config.get();
      evaluation = &retiring_rules;
    }
    if (!c)
      return false;
    bool allowed = false;
    for (size_t i = 0; i < c->sources_count; ++i) {
      const auto& s = c->sources[i];
      if (s.kind == mesh_interop_MatrixSource_Kind_GPIO_EDGE && s.gpio == e.pin &&
          !memcmp(s.node_mac, source_mac, 6))
        allowed = true;
    }
    for (size_t i = 0; i < c->conditions_count; ++i) {
      const auto& condition = c->conditions[i];
      if (condition.kind == mesh_interop_MatrixCondition_Kind_GPIO_STATE &&
          condition.gpio == e.pin && !memcmp(condition.node_mac, source_mac, 6))
        allowed = true;
    }
    if (!allowed)
      return false;
    if (memcmp(source_mac, mac, 6)) {
      const auto index = find_peer(source_mac);
      if (index >= 0 && e.pin <= 39) {
        auto& p = peers[index];
        if (e.at > p.gpio_state_at && (p.gpio_known & (1ULL << e.pin))) {
          if (e.rising)
            p.gpio_high |= 1ULL << e.pin;
          else
            p.gpio_high &= ~(1ULL << e.pin);
          p.gpio_state_at = e.at;
        }
      }
    }
    trigger_gpio_edge_t edge{};
    memcpy(edge.from_node_mac, source_mac, 6);
    edge.gpio_num = static_cast<gpio_num_t>(e.pin);
    edge.rising = e.rising;
    evaluation->tick(clock, &edge, 1, e.at, emit, this);
    return true;
  }
  void handle_gpio_event(peer& p, const uint8_t* b, size_t n, uint64_t now) {
    // The sender MAC comes from ESP-NOW, never from an event-supplied identity.
    if (n != 38 || w::get(b, 8) != session || !w::get(b + 24, 4) || b[37] > 1)
      return;
    const auto rev = w::get(b + 8, 8), stream = w::get(b + 16, 8);
    const uint32_t sequence = w::get(b + 24, 4);
    if (!accept_gpio_stream(p, stream))
      return;
    bool ok = true;
    if (sequence > p.gpio_sequence) {
      if (sequence != p.gpio_sequence + 1) {
        p.gpio_degraded = true;
        error("Slave GPIO events were lost");
        p.gpio_known = 0;
        std::vector<uint8_t> request;
        w::put(request, session, 8);
        send(p.mac, w::GPIO_STATE_REQUEST, request);
      }
      gpio_event e{w::get(b + 28, 8), b[36], b[37] != 0};
      const auto clock = uint64_t(master_clock_manager::instance().get_master_time());
      const bool valid_time =
          e.at <= clock + LEAD_US && (e.at >= clock || clock - e.at <= TRANSFER_TIMEOUT_US);
      // Bound stale/future input without replacing the original event timestamp.
      const bool superseded = ledger && rev < ledger->definitions().revision &&
                              (!retiring_ledger || rev != retiring_ledger->definitions().revision);
      ok = superseded || (valid_time && evaluate_gpio(p.mac, e, rev, clock));
      p.gpio_sequence = sequence;
      if (!ok) {
        p.gpio_degraded = true;
        p.gpio_known = 0;
        std::vector<uint8_t> request;
        w::put(request, session, 8);
        send(p.mac, w::GPIO_STATE_REQUEST, request);
        error("Slave GPIO event rejected (pin, revision, or time)");
      }
    }
    p.seen = now;
    std::vector<uint8_t> reply;
    w::put(reply, session, 8);
    w::put(reply, rev, 8);
    w::put(reply, stream, 8);
    w::put(reply, sequence, 4);
    w::put(reply, ok ? 0 : 1, 1);
    send(p.mac, w::GPIO_EVENT_ACK, reply);
  }
  void handle_radio(const packet& packet, uint64_t now) {
    if (!w::valid(packet.data, packet.size))
      return;
    const auto type = static_cast<w::type>(packet.data[5]);
    const auto* b = packet.data + w::HEADER_SIZE;
    const size_t n = packet.size - w::HEADER_SIZE;
    if (master) {
      if (!mesh_interop::instance().slave_node_exists(packet.mac))
        return;
      int index = find_peer(packet.mac);
      if (index < 0)
        return;
      auto& p = peers[index];
      if (type == w::SENSOR_BATCH) {
        accept_sensor_batch(packet.mac, b, n,
                            uint64_t(master_clock_manager::instance().get_master_time()));
        return;
      }
      if (!available(p, now))
        p.gpio_known = 0;
      if (type == w::INVENTORY && n >= 18 && w::get(b, 8) == session &&
          b[8] <= MAX_SOURCES_PER_NODE) {
        const size_t base_size = 17 + b[8] * 12;
        if (n < base_size + 1)
          return;
        const size_t version_length = b[base_size];
        if (version_length > 31 || n != base_size + 1 + version_length)
          return;
        for (size_t i = 0; i < version_length; ++i)
          if (b[base_size + 1 + i] < 0x20 || b[base_size + 1 + i] > 0x7e)
            return;
        std::array<mesh_interop_LayoutOutput, MAX_SOURCES_PER_NODE> outputs{};
        for (size_t i = 0; i < b[8]; ++i) {
          const auto* item = b + 9 + i * 12;
          const auto id = item[0], source = item[1], kind = item[2];
          const auto count = w::get(item + 4, 4), maximum = w::get(item + 8, 4);
          if (!id || id > MAX_DRIVERS_PER_NODE || source >= MAX_SOURCES_PER_NODE || kind > 2 ||
              item[3] > 1 || !count || count > 65536 || maximum < count || maximum > 65536)
            return;
          for (size_t j = 0; j < i; ++j)
            if (outputs[j].driver == id && outputs[j].source_index == source)
              return;
          auto& o = outputs[i];
          memcpy(o.node_mac, p.mac, 6);
          o.driver = static_cast<mesh_interop_MatrixDriver>(id);
          o.pixel_count = count;
          o.source_index = source;
          o.driver_kind = static_cast<mesh_interop_MatrixDriverKind>(kind);
          o.max_pixel_count = maximum;
          o.ready = item[3] != 0;
          o.known = true;
          o.online = true;
        }
        const auto mask = w::get(b + 9 + b[8] * 12, 8);
        if (mask >> 40)
          return;
        if (version_length)
          memcpy(p.firmware_version, b + base_size + 1, version_length);
        p.firmware_version[version_length] = '\0';
        p.gpio_mask = mask;
        p.gpio_known &= mask;
        p.outputs = outputs;
        p.outputs_count = b[8];
        p.catalog_known = true;
        p.catalog_at = now;
        default_layout_dirty = true;
      } else if (type == w::IDENTIFY_ACK && n == 13 && w::get(b, 8) == session &&
                 identifying.active && identifying.peer_index == index &&
                 identifying.wire_id == w::get(b + 8, 4)) {
        if (identifying.generation == ble_generation.load()) {
          if (b[12])
            layout_error(identifying.request_id, "Output is unavailable");
          else {
            mesh_interop_LayoutResponse response{};
            response.request_id = identifying.request_id;
            response.complete = true;
            publish_layout(response);
          }
        }
        identifying.active = false;
      } else if (type == w::GPIO_STATE && n == 44 && w::get(b, 8) == session) {
        const auto stream = w::get(b + 8, 8), timestamp = w::get(b + 20, 8);
        const auto mask = w::get(b + 28, 8), high = w::get(b + 36, 8);
        const auto clock = uint64_t(master_clock_manager::instance().get_master_time());
        if ((mask >> 40) || (high & ~mask) || !timestamp || timestamp > clock + LEAD_US ||
            (timestamp < clock && clock - timestamp > TRANSFER_TIMEOUT_US) ||
            !accept_gpio_stream(p, stream))
          return;
        if (p.catalog_known && (mask & ~p.gpio_mask))
          return;
        if (timestamp > p.gpio_state_at) {
          p.gpio_known = mask;
          p.gpio_high = high;
          p.gpio_state_at = timestamp;
          p.gpio_snapshot_seen = now;
        }
        p.seen = now;
        std::vector<uint8_t> reply;
        w::put(reply, session, 8);
        w::put(reply, stream, 8);
        w::put(reply, timestamp, 8);
        send(p.mac, w::GPIO_STATE_ACK, reply);
      } else if (type == w::GPIO_EVENT)
        handle_gpio_event(p, b, n, now);
      else if (type == w::ACK)
        transfer_ack(index, b, n);
      else if (type == w::COMMAND_ACK && n == 20 && w::get(b, 8) == session) {
        if (!p.retiring_commands.empty() &&
            w::get(b + 8, 8) == p.retiring_commands.front().revision &&
            w::get(b + 16, 4) == p.retiring_commands.front().sequence) {
          p.retiring_commands.pop_front();
          p.retiring_retries = 0;
          p.retiring_retry = 0;
        } else if (!p.commands.empty() && w::get(b + 8, 8) == p.commands.front().revision &&
                   w::get(b + 16, 4) == p.commands.front().sequence) {
          p.commands.pop_front();
          p.retries = 0;
          p.last_retry = 0;
        }
      } else if (type == w::HELLO && n == 29 && w::get(b, 8) == session) {
        if (!accept_gpio_stream(p, w::get(b + 20, 8)))
          return;
        if (b[28]) {
          p.gpio_degraded = true;
          error("Slave reported GPIO event loss");
        }
        p.seen = now;
        const auto running_revision = ledger ? ledger->definitions().revision : revision;
        if (p.ready && (w::get(b + 8, 8) != running_revision || w::get(b + 16, 4) > p.sequence ||
                        (p.commands.empty() && w::get(b + 16, 4) != p.sequence))) {
          p.ready = false;
          p.definitions = false;
          p.commands.clear();
        }
      } else if (type == w::SNAPSHOT_REQUEST) {
        p.ready = false;
        p.snapshotting = false;
        p.commands.clear();
        if (tx.peer_index == index && tx.kind == 2)
          tx = outbound{};
        p.last_recover = 0;
      }
      return;
    }
    if (!mesh_interop::instance().master_node_matches(packet.mac) ||
        !master_clock_manager::instance().is_synchronized())
      return;
    if (type == w::SIGNAL_BATCH) {
      accept_signal_batch(b, n, uint64_t(master_clock_manager::instance().get_master_time()));
    } else if (type == w::GPIO_STATE_REQUEST && n == 8 && w::get(b, 8) == session) {
      gpio_snapshot.clear();
      gpio_snapshot_acked = 0;
      gpio_snapshot_last_send = 0;
    } else if (type == w::GPIO_STATE_ACK && n == 24 && w::get(b, 8) == session &&
               w::get(b + 8, 8) == gpio_stream && w::get(b + 16, 8) == gpio_snapshot_at) {
      gpio_snapshot_acked = now;
      gpio_snapshot.clear();
    } else if (type == w::INVENTORY_REQUEST && n == 8 && w::get(b, 8) == session) {
      send_inventory(packet.mac);
    } else if (type == w::IDENTIFY && n == 25 && w::get(b, 8) == session && w::get(b + 8, 4) &&
               b[24] <= 1) {
      const uint32_t id = w::get(b + 8, 4), driver = w::get(b + 12, 4);
      const uint32_t source = w::get(b + 16, 4);
      const uint32_t length = w::get(b + 20, 4);
      const bool reversed = b[24];
      bool ok;
      if (id == last_identify_id)
        ok = last_identify_driver == driver && last_identify_source == source &&
             last_identify_length == length && last_identify_reversed == reversed &&
             last_identify_ok;
      else {
        ok = begin_identify(driver, now, source, length, reversed);
        last_identify_id = id;
        last_identify_driver = driver;
        last_identify_source = source;
        last_identify_ok = ok;
        last_identify_length = length;
        last_identify_reversed = reversed;
      }
      std::vector<uint8_t> reply;
      w::put(reply, session, 8);
      w::put(reply, id, 4);
      w::put(reply, ok ? 0 : 1, 1);
      send(packet.mac, w::IDENTIFY_ACK, reply);
    } else if (type == w::GPIO_EVENT_ACK && n == 29 && !gpio_events.empty() &&
               w::get(b, 8) == session && w::get(b + 16, 8) == gpio_stream &&
               w::get(b + 8, 8) == gpio_events.front().revision &&
               w::get(b + 24, 4) == gpio_events.front().sequence) {
      if (b[28]) {
        gpio_degraded = true;
        gpio_snapshot.clear();
        gpio_snapshot_acked = 0;
        error("Master rejected GPIO event");
        last_hello = 0;
      }
      gpio_events.pop_front();
      gpio_last_send = 0;
      gpio_retries = 0;
    } else if (type == w::BEGIN && n == 29) {
      const auto id = uint32_t(w::get(b, 4));
      bool ok = (b[4] == 1 || b[4] == 2 || b[4] == 3) && w::get(b + 21, 8) == session;
      if (radio_rx.id() == id && (inbound_kind != b[4] || inbound_revision != w::get(b + 13, 8)))
        ok = false;
      if (local && w::get(b + 13, 8) < local->definitions().revision)
        ok = false;
      if (pending_player && w::get(b + 13, 8) < pending_player->definitions().revision)
        ok = false;
      if (ok)
        ok = radio_rx.begin(id, w::get(b + 5, 4), w::get(b + 9, 4), now);
      if (ok) {
        inbound_kind = b[4];
        inbound_revision = w::get(b + 13, 8);
      }
      acknowledge(packet.mac, id, 0, ok);
    } else if (type == w::CHUNK && n > 8 && n <= 208) {
      uint32_t id = w::get(b, 4);
      bool ok = radio_rx.append(id, w::get(b + 4, 4), b + 8, n - 8, now);
      // A missing/out-of-order chunk gets an ACK of the expected offset.
      acknowledge(packet.mac, id, 1, ok || id == radio_rx.id());
    } else if (type == w::FINISH && n == 4) {
      uint32_t id = w::get(b, 4);
      bool ok = id == completed_transfer || (radio_rx.complete(id) && install_received());
      if (ok)
        completed_transfer = id;
      acknowledge(packet.mac, id, 2, ok);
    } else if (type == w::COMMAND) {
      animation_command c{};
      if (!decode(mesh_interop_AnimationCommand_fields, &c, b, n) || c.master_session != session)
        return;
      const bool previous = local && pending_snapshot && pending_player &&
                            c.revision == local->definitions().revision &&
                            c.revision != pending_player->definitions().revision;
      auto* p = previous                             ? local.get()
                : pending_snapshot && pending_player ? pending_player.get()
                                                     : local.get();
      if (!p || c.revision != p->definitions().revision)
        return;
      auto& sequence = previous ? previous_sequence : local_sequence;
      if (c.sequence == sequence + 1 && p->enqueue(c))
        ++sequence;
      else if (c.sequence > sequence) {
        send(packet.mac, w::SNAPSHOT_REQUEST, {});
        return;
      }
      std::vector<uint8_t> reply;
      w::put(reply, session, 8);
      w::put(reply, c.revision, 8);
      w::put(reply, c.sequence, 4);
      send(packet.mac, w::COMMAND_ACK, reply);
    }
  }
  static bool emit(void* arg, const animation_command& input) {
    auto& s = *static_cast<service*>(arg);
    auto command = input;
    const bool previous =
        s.retiring_ledger && command.revision == s.retiring_ledger->definitions().revision;
    if (previous && command.effective_at_us >= s.activate_at)
      return true;
    auto* timeline = previous ? s.retiring_ledger.get() : s.ledger.get();
    auto* config = previous ? s.retiring_config.get() : s.config.get();
    if (config && config->factory_default)
      command.effective_at_us = 0;
    if (!timeline || !timeline->enqueue(command)) {
      s.error("Animation timeline queue overflow");
      return false;
    }
    const mesh_interop_MatrixEntry* entry = nullptr;
    for (size_t i = 0; i < config->entries_count; ++i)
      if (config->entries[i].id == command.entry_id)
        entry = &config->entries[i];
    if (!entry)
      return false;
    for (size_t j = 0; j < config->layout_count; ++j) {
      auto& target = config->layout[j];
      bool handled = false;
      for (size_t k = 0; k < j; ++k)
        if (!memcmp(target.node_mac, config->layout[k].node_mac, 6))
          handled = true;
      if (handled)
        continue; // One command drives all this entry's local outputs.
      if (!memcmp(target.node_mac, s.mac, 6)) {
        auto* p = previous             ? s.local.get()
                  : s.pending_snapshot ? s.pending_player.get()
                                       : s.local.get();
        if (p && !p->enqueue(command))
          s.error("Local animation queue overflow");
      } else {
        int index = s.find_peer(target.node_mac);
        if (index < 0)
          continue;
        auto& p = s.peers[index];
        if (previous) {
          if (p.retiring_commands.size() < 64) {
            auto c = command;
            c.sequence = ++p.retiring_sequence;
            p.retiring_commands.push_back(c);
          } else
            s.error("Retiring command queue overflow");
          continue;
        }
        if (!p.ready && !p.snapshotting)
          continue;
        if (p.commands.size() == 64 || p.sequence == UINT32_MAX) {
          p.ready = false;
          p.snapshotting = false;
          p.commands.clear();
          if (s.tx.peer_index == index && s.tx.kind == 2)
            s.tx = outbound{};
          p.last_recover = 0;
          s.error("Node command queue overflow; recovering snapshot");
          continue;
        }
        auto c = command;
        c.sequence = ++p.sequence;
        p.commands.push_back(c);
      }
    }
    return true;
  }
  void activate(uint64_t now) {
    auto next_snapshot = make_heap_object<animation_snapshot>();
    if (!prepared_ledger || !prepared_local || !next_snapshot) {
      error("Unable to allocate replacement animation runtime", true);
      deployment = false;
      return;
    }
    retiring_config = std::move(config);
    retiring_ledger = std::move(ledger);
    retiring_rules = rules;
    config = std::move(candidate);
    ledger = std::move(prepared_ledger);
    for (auto& p : peers) {
      p.retiring_commands = std::move(p.commands);
      p.retiring_sequence = p.sequence;
      p.retiring_retry = p.last_retry;
      p.retiring_retries = p.retries;
      p.ready = false;
      p.definitions = p.replacement_prepared;
      p.snapshotting = false;
      p.commands.clear();
      p.sequence = 0;
      p.last_retry = 0;
      p.retries = 0;
      p.last_recover = 0;
    }
    pending_player = std::move(prepared_local);
    pending_snapshot = std::move(next_snapshot);
    // Configuration rollout can take longer than one trigger's 100 ms lead.
    // Reserve a shared epoch far enough ahead to transfer initial snapshots.
    activate_at = now + LEAD_US + peer_count * 500000;
    pending_snapshot->activate_at_us = activate_at;
    const bool mode_switched =
        retiring_config && retiring_config->active_mode_id != config->active_mode_id;
    rules.set_gpio_lookup(lookup_gpio, this);
    rules.configure(config.get(), session, revision, boot_pending, mode_switched);
    boot_pending = false;
    rules.tick(activate_at - LEAD_US, nullptr, 0, now, emit, this);
    deployment = false;
  }
  void cleanup_gpio(bool keep_pending = false) {
    (void)keep_pending;
    // Provisioned inputs stay initialized so boot and reconnect snapshots do
    // not depend on whether animation definitions have arrived yet.
  }
  void collect_gpio(uint64_t clock) {
    if (gpio_overflow.exchange(false)) {
      gpio_degraded = true;
      last_hello = 0;
      error("GPIO capture queue overflow; edges lost");
      gpio_snapshot.clear();
      gpio_snapshot_acked = 0;
    }
    gpio_event e;
    for (unsigned i = 0; i < 64 && xQueueReceive(gpio_queue, &e, 0) == pdTRUE; ++i) {
      // ISR timestamps are local monotonic time. Apply the current synchronized
      // offset in task context, retaining queue latency rather than using arrival time.
      if (e.at > uint64_t(esp_timer_get_time()))
        continue;
      const auto timestamp = master_clock_manager::instance().get_master_time_at(e.at);
      if (timestamp < 0)
        continue;
      e.at = uint64_t(timestamp);
      const auto* p = pending_snapshot && pending_player && e.at >= pending_snapshot->activate_at_us
                          ? pending_player.get()
                          : local.get();
      if (!p)
        continue;
      const auto& d = p->definitions();
      if (std::find(d.gpio_inputs.begin(), d.gpio_inputs.begin() + d.gpio_inputs_count, e.pin) ==
          d.gpio_inputs.begin() + d.gpio_inputs_count)
        continue;
      if (master) {
        if (e.rising)
          local_gpio_high |= 1ULL << e.pin;
        else
          local_gpio_high &= ~(1ULL << e.pin);
        evaluate_gpio(mac, e, d.revision, clock);
      } else if (gpio_events.size() < 64 && gpio_sequence != UINT32_MAX) {
        gpio_events.push_back({e, d.revision, ++gpio_sequence});
      } else {
        gpio_degraded = true;
        last_hello = 0;
        error("GPIO delivery queue overflow; edges lost");
        gpio_snapshot.clear();
        gpio_snapshot_acked = 0;
      }
    }
  }
  void gpio_send_step(const uint8_t* dest, uint64_t now) {
    if (gpio_events.empty() || (gpio_last_send && now - gpio_last_send < 100000))
      return;
    if (gpio_retries++ >= 4) {
      gpio_events.pop_front();
      gpio_retries = 0;
      gpio_last_send = 0;
      gpio_snapshot.clear();
      gpio_snapshot_acked = 0;
      gpio_degraded = true;
      last_hello = 0;
      error("GPIO event timed out; edge lost");
      return;
    }
    const auto& e = gpio_events.front();
    std::vector<uint8_t> b;
    w::put(b, session, 8);
    w::put(b, e.revision, 8);
    w::put(b, gpio_stream, 8);
    w::put(b, e.sequence, 4);
    w::put(b, e.edge.at, 8);
    w::put(b, e.edge.pin, 1);
    w::put(b, e.edge.rising, 1);
    send(dest, w::GPIO_EVENT, b);
    gpio_last_send = now;
  }
  [[gnu::noinline]] void master_step(uint64_t now, uint64_t clock) {
    local_gpio_known = initialized_input_mask();
    local_gpio_high = read_input_levels(local_gpio_known);
    auto mesh = mesh_interop::instance().get_state();
    for (size_t i = 0; i < mesh.nodes_count; ++i) {
      int idx = find_peer(mesh.nodes[i].mac);
      if (idx >= 0 && !available(peers[idx], now))
        peers[idx].gpio_known = 0;
      if (idx >= 0 && mesh.nodes[i].last_synced_at)
        peers[idx].seen = std::max(peers[idx].seen, mesh.nodes[i].last_synced_at);
    }
    for (size_t i = 0; i < peer_count; ++i) {
      auto& p = peers[i];
      if (p.gpio_snapshot_seen && now > p.gpio_snapshot_seen &&
          now - p.gpio_snapshot_seen >= 4000000)
        p.gpio_known = 0;
      if (available(p, now) && !p.gpio_known &&
          (!p.gpio_request_at || now - p.gpio_request_at >= 1000000)) {
        std::vector<uint8_t> request;
        w::put(request, session, 8);
        send(p.mac, w::GPIO_STATE_REQUEST, request);
        p.gpio_request_at = now;
      }
      if (available(p, now) && (!p.catalog_at || now - p.catalog_at >= 5000000) &&
          (!p.catalog_retry || now - p.catalog_retry >= 1000000)) {
        std::vector<uint8_t> body;
        w::put(body, session, 8);
        send(p.mac, w::INVENTORY_REQUEST, body);
        p.catalog_retry = now;
      }
    }
    if (identifying.active) {
      if (identifying.generation != ble_generation.load())
        identifying.active = false;
      else if (!identifying.sent_at || now - identifying.sent_at >= 100000) {
        if (identifying.retries++ >= 4) {
          layout_error(identifying.request_id, "Output did not respond; try again");
          identifying.active = false;
        } else {
          std::vector<uint8_t> body;
          w::put(body, session, 8);
          w::put(body, identifying.wire_id, 4);
          w::put(body, identifying.driver, 4);
          w::put(body, identifying.source, 4);
          w::put(body, identifying.length, 4);
          w::put(body, identifying.reversed, 1);
          send(peers[identifying.peer_index].mac, w::IDENTIFY, body);
          identifying.sent_at = now;
        }
      }
    }
    extend_default_layout();
    if (preview_distributing && tx.peer_index < 0) {
      bool started = false;
      for (size_t i = 0; i < peer_count; ++i) {
        auto& peer = peers[i];
        if (peer.preview_attempted || !peer.ready || !available(peer, now))
          continue;
        const bool mapped =
            std::any_of(config->layout.begin(), config->layout.end(),
                        [&](const auto& row) { return !memcmp(row.node_mac, peer.mac, 6); });
        peer.preview_attempted = true;
        if (!mapped)
          continue;
        begin_transfer(i, 3, preview_bytes);
        started = true;
        break;
      }
      if (!started) {
        preview_distributing = false;
        preview_bytes.clear();
      }
    }
    if (deployment && tx.peer_index < 0) {
      bool started = false;
      for (size_t i = 0; i < peer_count; ++i) {
        auto& p = peers[i];
        if (p.attempted || !available(p, now))
          continue;
        if (candidate->factory_default &&
            std::none_of(candidate->layout.begin(), candidate->layout.end(),
                         [&](const auto& row) { return !memcmp(row.node_mac, p.mac, 6); }))
          continue;
        auto d = make_definitions(*candidate, p.mac, session, revision);
        if (!d) {
          p.attempted = true;
          error("Unable to allocate node definitions");
          continue;
        }
        begin_transfer(i, 1, encode(mesh_interop_AnimationDefinitions_fields, d.get()));
        started = true;
        break;
      }
      if (!started) {
        const bool has_boot =
            boot_pending &&
            std::any_of(candidate->sources.begin(), candidate->sources.end(),
                        [](const auto& source) {
                          return source.kind == mesh_interop_MatrixSource_Kind_ON_BOOT;
                        });
        bool inputs_waiting = false;
        if (has_boot)
          for (const auto& condition : candidate->conditions) {
            if (condition.kind != mesh_interop_MatrixCondition_Kind_GPIO_STATE ||
                !memcmp(condition.node_mac, mac, 6))
              continue;
            const int index = find_peer(condition.node_mac);
            if (index >= 0 && !(peers[index].gpio_known & (1ULL << condition.gpio)))
              inputs_waiting = true;
          }
        const bool waiting = has_boot && (mesh_interop::instance().startup_waiting_for_nodes(now) ||
                                          (inputs_waiting && now < startup_at + 2000000));
        if (!waiting)
          activate(clock);
      }
    }
    if (config && ledger && (!last_signal_batch || now - last_signal_batch >= 16667)) {
      if (signal_sources)
        signal_sources->retain(ledger->definitions().revision,
                               retiring_ledger ? retiring_ledger->definitions().revision : 0);
      signal_values.retain(ledger->definitions().revision,
                           retiring_ledger ? retiring_ledger->definitions().revision : 0);
      send_signal_batches(*config, ledger->definitions().revision, clock);
      if (retiring_config && retiring_ledger && clock < activate_at)
        send_signal_batches(*retiring_config, retiring_ledger->definitions().revision, clock);
      last_signal_batch = now;
    }
    collect_gpio(clock);
    local_gpio_high = read_input_levels(local_gpio_known);
    if (config && ledger) {
      if (retiring_config && clock < activate_at) {
        retiring_rules.tick(clock, nullptr, 0, clock, emit, this);
        retiring_ledger->tick(clock, false);
      }
      if (clock >= activate_at)
        rules.tick(clock, nullptr, 0, clock, emit, this);
      ledger->tick(clock, false);
      for (size_t i = 0; i < peer_count; ++i) {
        auto& p = peers[i];
        if (!available(p, now)) {
          p.ready = false;
          p.commands.clear();
          continue;
        }
        if (config->factory_default &&
            std::none_of(config->layout.begin(), config->layout.end(),
                         [&](const auto& row) { return !memcmp(row.node_mac, p.mac, 6); }))
          continue;
        if (clock < activate_at && !p.retiring_commands.empty() &&
            (!p.retiring_retry || now - p.retiring_retry >= 100000)) {
          if (p.retiring_retries++ < 4) {
            send(p.mac, w::COMMAND,
                 encode(mesh_interop_AnimationCommand_fields, &p.retiring_commands.front()));
            p.retiring_retry = now;
          } else
            p.retiring_commands.clear();
        }
        if (p.ready && !p.commands.empty() && (!p.last_retry || now - p.last_retry >= 100000)) {
          if (p.retries++ >= 4) {
            p.ready = false;
            p.commands.clear();
            error("Animation command timed out; recovering snapshot");
          } else {
            send(p.mac, w::COMMAND,
                 encode(mesh_interop_AnimationCommand_fields, &p.commands.front()));
            p.last_retry = now;
          }
        }
        if (!deployment && !p.ready && !p.snapshotting && tx.peer_index < 0 &&
            now - p.last_recover >= 1000000) {
          auto d = make_definitions(*config, p.mac, session, revision);
          if (!d) {
            p.last_recover = now;
            error("Unable to allocate node definitions");
            continue;
          }
          if (!p.definitions)
            begin_transfer(i, 1, encode(mesh_interop_AnimationDefinitions_fields, d.get()));
          else {
            p.commands.clear();
            p.retries = 0;
            p.last_retry = 0;
            auto snapshot = make_heap_object<animation_snapshot>();
            if (!snapshot) {
              p.last_recover = now;
              error("Unable to allocate recovery snapshot");
              continue;
            }
            *snapshot = ledger->snapshot(*d, p.sequence, activate_at);
            begin_transfer(i, 2, encode(mesh_interop_AnimationSnapshot_fields, snapshot.get()));
          }
        }
      }
    }
    state.nodes_count = 0;
    auto& self = state.nodes[state.nodes_count++];
    memcpy(self.node_mac, mac, 6);
    self.state = gpio_degraded ? NODE_DEGRADED : NODE_READY;
    for (size_t i = 0; i < peer_count; ++i) {
      auto& s = state.nodes[state.nodes_count++];
      auto& p = peers[i];
      memcpy(s.node_mac, p.mac, 6);
      s.state = !available(p, now) ? NODE_UNAVAILABLE
                : p.ready          ? (p.gpio_degraded ? NODE_DEGRADED : NODE_READY)
                : (tx.peer_index == int(i) || deployment) ? NODE_PREPARING
                                                          : NODE_DEGRADED;
    }
  }
  void reset_slave_playback() {
    preview_player.reset();
    for (uint32_t id = 1; id <= connected_lighting_drivers::MAX_DRIVERS; ++id) {
      auto* driver = connected_lighting_drivers::find(id);
      if (driver && driver->_is_ready()) {
        driver->clear();
        driver->show();
      }
    }
    local.reset();
    pending_player.reset();
    pending_snapshot.reset();
    radio_rx.reset();
    completed_transfer = 0;
    local_sequence = 0;
    revision = 0;
    identify_driver = 0;
    gpio_events.clear();
    last_hello = 0;
  }
  [[gnu::noinline]] void slave_step(uint64_t now) {
    auto& clock = master_clock_manager::instance();
    if (!slave_started_at)
      slave_started_at = now;
    const auto last_sync = uint64_t(clock.last_sync_at());
    const auto mesh = mesh_interop::instance().get_state();
    if (mesh.is_discovery_mode) {
      if (!discovering) {
        reset_slave_playback();
        discovery_last_sync = last_sync;
        discovering = true;
        waiting_for_discovery_sync = true;
        session = 0;
      }
      if (standby) {
        const auto err = mesh_interop::instance().set_standby_radio(false);
        if (err != ESP_OK) {
          ESP_LOGW("matrix", "Unable to wake radio for discovery: %s", esp_err_to_name(err));
          return;
        }
        standby = false;
      }
      gpio_event ignored;
      while (xQueueReceive(gpio_queue, &ignored, 0) == pdTRUE) {
      }
      return;
    }
    if (discovering) {
      discovering = false;
      slave_started_at = now;
      session = 0;
    }
    if (waiting_for_discovery_sync && last_sync > discovery_last_sync)
      waiting_for_discovery_sync = false;
    if (!last_sync && !mesh.nodes_count) {
      slave_started_at = now;
      return; // Keep an unpaired node available for discovery.
    }
    const auto last_contact =
        waiting_for_discovery_sync ? slave_started_at : (last_sync ? last_sync : slave_started_at);
    if (!standby && now >= last_contact && now - last_contact >= STANDBY_TIMEOUT_US) {
      reset_slave_playback();
      standby_last_sync = last_sync;
      standby = true;
      auto err = mesh_interop::instance().set_standby_radio(true);
      if (err != ESP_OK)
        ESP_LOGW("matrix", "Unable to enable standby radio: %s", esp_err_to_name(err));
      ESP_LOGI("matrix", "Controller timed out; outputs off and node in standby");
    }
    if (standby) {
      gpio_event ignored;
      while (xQueueReceive(gpio_queue, &ignored, 0) == pdTRUE) {
      }
      if (!last_sync || last_sync <= standby_last_sync || now < last_sync ||
          now - last_sync >= STANDBY_TIMEOUT_US)
        return;
      auto err = mesh_interop::instance().set_standby_radio(false);
      if (err != ESP_OK) {
        ESP_LOGW("matrix", "Unable to wake radio: %s", esp_err_to_name(err));
        return;
      }
      standby = false;
      session = 0; // Force a fresh HELLO and snapshot, even for the same controller session.
      ESP_LOGI("matrix", "Controller heartbeat received; recovering playback");
    }
    if (waiting_for_discovery_sync)
      return;
    if (!clock.is_synchronized()) {
      gpio_event e;
      for (unsigned i = 0; i < 64 && xQueueReceive(gpio_queue, &e, 0) == pdTRUE; ++i) {
      }
      return;
    }
    const auto observed = clock.session();
    if (session != observed) {
      preview_player.reset();
      signal_values.clear();
      if (local)
        local->blank();
      session = observed;
      local.reset();
      pending_player.reset();
      pending_snapshot.reset();
      radio_rx.reset();
      completed_transfer = 0;
      local_sequence = 0;
      revision = 0;
      gpio_events.clear();
      gpio_sequence = 0;
      gpio_stream = 0;
      gpio_snapshot.clear();
      gpio_snapshot_acked = gpio_snapshot_last_send = gpio_snapshot_at = 0;
      gpio_retries = 0;
      gpio_last_send = 0;
      gpio_degraded = false;
      last_hello = 0;
      if (identify_driver) {
        clear_identified_source();
      }
      identify_driver = 0;
      identify_source = 0;
      last_identify_id = 0;
      cleanup_gpio();
      gpio_event e;
      for (unsigned i = 0; i < 64 && xQueueReceive(gpio_queue, &e, 0) == pdTRUE; ++i) {
      }
    }
    if (!gpio_stream) {
      gpio_stream = (uint64_t(esp_random()) << 32) | esp_random();
      if (!gpio_stream)
        gpio_stream = 1;
    }
    collect_gpio(uint64_t(clock.get_master_time()));
    if (mesh.nodes_count) {
      if (!gpio_snapshot.empty() && now - gpio_snapshot_started >= 1000000)
        gpio_snapshot.clear();
      if (gpio_snapshot.empty() && (!gpio_snapshot_acked || now - gpio_snapshot_acked >= 1000000)) {
        gpio_snapshot_at = uint64_t(clock.get_master_time());
        if (!gpio_snapshot_at)
          gpio_snapshot_at = 1;
        const auto mask = initialized_input_mask();
        w::put(gpio_snapshot, session, 8);
        w::put(gpio_snapshot, gpio_stream, 8);
        w::put(gpio_snapshot, gpio_sequence, 4);
        w::put(gpio_snapshot, gpio_snapshot_at, 8);
        w::put(gpio_snapshot, mask, 8);
        w::put(gpio_snapshot, read_input_levels(mask), 8);
        gpio_snapshot_last_send = 0;
        gpio_snapshot_started = now;
      }
      if (!gpio_snapshot.empty() &&
          (!gpio_snapshot_last_send || now - gpio_snapshot_last_send >= 100000)) {
        send(mesh.nodes[0].mac, w::GPIO_STATE, gpio_snapshot);
        gpio_snapshot_last_send = now;
      }
      gpio_send_step(mesh.nodes[0].mac, now);
    }
    if (now - last_hello >= 1000000) {
      if (mesh.nodes_count) {
        std::vector<uint8_t> b;
        w::put(b, session, 8);
        auto* advertised = pending_snapshot && pending_player ? pending_player.get() : local.get();
        w::put(b, advertised ? advertised->definitions().revision : 0, 8);
        w::put(b, local_sequence, 4);
        w::put(b, gpio_stream, 8);
        w::put(b, gpio_degraded ? 1 : 0, 1);
        send(mesh.nodes[0].mac, w::HELLO, b);
      }
      last_hello = now;
    }
  }
#ifdef MATRIX_MEMORY_STRESS_TEST
  [[gnu::noinline]] void test_preview_stack() {
    if (!config || config->effects.empty() || !local ||
        !local->definitions().output_mappings_count) {
      ESP_LOGW("stack_test", "SKIP: no active effect with a local output");
      return;
    }
    auto request = make_heap_object<mesh_interop_MatrixUpload>();
    auto input = make_heap_object<packet>();
    if (!request || !input) {
      ESP_LOGE("stack_test", "FAIL: unable to allocate preview request");
      return;
    }
    request->which_operation = mesh_interop_MatrixUpload_layout_request_tag;
    request->operation.layout_request.request_id = 1;
    request->operation.layout_request.operation = mesh_interop_LayoutRequest_Operation_PREVIEW;
    request->operation.layout_request.has_identify_options = true;
    request->operation.layout_request.identify_options = config->effects[0].id;
    const auto bytes = encode(mesh_interop_MatrixUpload_fields, request.get());
    if (bytes.empty() || bytes.size() > sizeof(input->data)) {
      ESP_LOGE("stack_test", "FAIL: unable to encode preview request");
      return;
    }
    input->connection = 1;
    input->generation = ble_generation.load();
    input->size = bytes.size();
    memcpy(input->data, bytes.data(), bytes.size());
    handle_upload(*input, esp_timer_get_time());
    const auto headroom = uxTaskGetStackHighWaterMark(nullptr);
    if (!preview_player || headroom < 1024) {
      ESP_LOGE("stack_test", "FAIL: preview=%u stack headroom=%u bytes",
               unsigned(bool(preview_player)), unsigned(headroom));
      return;
    }
    ESP_LOGI("stack_test", "PASS: preview request, matrix stack headroom=%u bytes",
             unsigned(headroom));
  }
#endif
  [[gnu::noinline]] void initialize_provisioned_gpio() {
    playback_definitions provisioned;
    for (uint32_t pin = 0; pin < 40; ++pin)
      if (valid_input(pin))
        provisioned.gpio_inputs[provisioned.gpio_inputs_count++] = pin;
    if (!prepare_gpio(provisioned))
      error("Unable to initialize provisioned GPIO inputs");
  }
  void run() {
    master = global_config::instance().is_master_node();
    memcpy(mac, global_config::instance().get_mesh_mac(), 6);
    session = master_clock_manager::instance().session();
    state.master_session = session;
    memcpy(state.master_mac, mac, 6);
    startup_at = esp_timer_get_time();
    initialize_provisioned_gpio();
    rules.set_gpio_lookup(lookup_gpio, this);
    initialize_configuration();
    log_memory();
    TickType_t next_frame = xTaskGetTickCount();
#ifdef MATRIX_MEMORY_STRESS_TEST
    bool memory_test_done = false;
#endif
    for (;;) {
      uint64_t now = esp_timer_get_time();
#ifdef MATRIX_MEMORY_STRESS_TEST
      if (master && !memory_test_done && now - startup_at >= 10000000 &&
          (config || now - startup_at >= 20000000)) {
        memory_test_done = true;
        run_memory_stress_test(config.get(), mac, connected_lighting_drivers::find,
                               decode_compact_config);
        test_preview_stack();
      }
#endif
      if (!master)
        slave_step(now);
      packet p;
      for (unsigned i = 0; i < 16 && xQueueReceive(priority_queue, &p, 0) == pdTRUE; ++i)
        if (!standby && !discovering && !waiting_for_discovery_sync)
          handle_radio(p, now);
      for (unsigned i = 0; i < 8 && xQueueReceive(radio_queue, &p, 0) == pdTRUE; ++i)
        if (!standby && !discovering && !waiting_for_discovery_sync)
          handle_radio(p, now);
      signal_packet signal_frame{};
      for (unsigned i = 0; i < 4 && xQueueReceive(signal_queue, &signal_frame, 0) == pdTRUE; ++i) {
        if (!standby && !discovering && !waiting_for_discovery_sync) {
          memcpy(p.mac, signal_frame.mac, 6);
          p.size = signal_frame.size;
          memcpy(p.data, signal_frame.data, p.size);
          handle_radio(p, now);
        }
      }
      if (ble_rx.id() && upload_generation != ble_generation.load()) {
        ble_rx.reset();
        error("Upload connection closed", true);
      }
      if (xQueueReceive(ble_queue, &p, 0) == pdTRUE)
        handle_upload(p, now);
      uint32_t old_id = ble_rx.id();
      ble_rx.expire(now);
      radio_rx.expire(now);
      if (old_id && !ble_rx.id())
        error("Upload timed out", true);
      auto clock = static_cast<uint64_t>(master_clock_manager::instance().get_master_time());
      if (!standby && !discovering && !waiting_for_discovery_sync)
        collect_sensor_samples(clock);
      if (master)
        master_step(now, clock);
      transfer_step(now);
      const TickType_t ticks = xTaskGetTickCount();
      if (static_cast<int32_t>(ticks - next_frame) >= 0) {
        playback_step(clock);
        next_frame = ticks + pdMS_TO_TICKS(10);
      }
      if (overflow.exchange(false)) {
        error("Transport queue overflow");
        if (master) {
          for (size_t i = 0; i < peer_count; ++i) {
            peers[i].gpio_known = 0;
            peers[i].gpio_request_at = 0;
          }
        } else {
          gpio_snapshot.clear();
          gpio_snapshot_acked = 0;
          auto mesh = mesh_interop::instance().get_state();
          if (mesh.nodes_count)
            send(mesh.nodes[0].mac, w::SNAPSHOT_REQUEST, {});
        }
      }
      publish();
      // ACKs wake the worker between render frames. A 10 ms sleep per packet
      // would make a simultaneous multi-layer trigger miss its 100 ms lead.
      const TickType_t current = xTaskGetTickCount();
      const int32_t remaining = static_cast<int32_t>(next_frame - current);
      ulTaskNotifyTake(pdTRUE, remaining > 0 ? remaining : 0);
    }
  }
  [[gnu::noinline]] void playback_step(uint64_t clock) {
    if (standby || discovering || waiting_for_discovery_sync)
      return;
    signal_values.retain(local ? local->definitions().revision : 0,
                         pending_player ? pending_player->definitions().revision : 0);
    if (pending_snapshot && pending_player && clock >= pending_snapshot->activate_at_us) {
      if (local)
        local->blank();
      local = std::move(pending_player);
      revision = local->definitions().revision;
      active_persisted = configuration_persisted;
      if (!master)
        config.reset();
      pending_snapshot.reset();
      retiring_config.reset();
      retiring_ledger.reset();
      for (auto& p : peers)
        p.retiring_commands.clear();
      cleanup_gpio();
      if (master)
        state.stage = STAGE_ACTIVE;
      ESP_LOGI("matrix", "Activated revision %llu", static_cast<unsigned long long>(revision));
      log_memory();
    }
    const uint64_t now = esp_timer_get_time();
    const bool synchronized = master_clock_manager::instance().is_synchronized();
    if (identify_driver && now >= identify_until) {
      bool mapped = false;
      if (local && synchronized)
        for (size_t i = 0; i < local->definitions().output_mappings_count; ++i)
          if (local->definitions().output_mappings[i].driver == identify_driver &&
              local->definitions().output_mappings[i].source_index == identify_source)
            mapped = true;
      if (!mapped) {
        clear_identified_source();
      }
      identify_driver = 0;
    }
    if (preview_player && (clock >= preview_end || !local ||
                           local->definitions().revision != preview_player->definitions().revision))
      preview_player.reset();
    const bool previewing = preview_player && clock >= preview_start && clock < preview_end;
    if (local && synchronized) {
      local->set_signal_lookup(lookup_signal, this);
      auto err = local->tick(clock, !previewing && identify_driver != IDENTIFY_ALL_DRIVERS,
                             identify_driver, identify_source);
      if (err != ESP_OK && !state.error[0])
        error("Animation driver failed");
    }
    if (previewing && synchronized) {
      preview_player->set_signal_lookup(lookup_signal, this);
      preview_player->tick(clock, identify_driver != IDENTIFY_ALL_DRIVERS, identify_driver,
                           identify_source);
    }
    if (identify_driver) {
      const auto draw = [&](rgb_lighting_driver* driver, uint32_t source_index, bool all) {
        const auto capacity = driver->get_layout_pixel_count(source_index);
        if (!capacity)
          return;
        linear_lighting_driver_view source(*driver, 0, capacity, capacity, source_index);
        source.clear();
        const uint64_t elapsed = 3000000 - (identify_until - now);
        const uint32_t length = all ? capacity : identify_length;
        if (driver->get_layout_source_kind(source_index) == lighting_driver_kind::RGB_LED_MODULE) {
          if (elapsed / 250000 % 2 == 0)
            source.set_pixel(0, alpha_color(color(0, 220, 233)));
        } else {
          const uint32_t filled = all                  ? (elapsed / 250000 % 2 == 0 ? length : 0)
                                  : elapsed >= 2000000 ? length
                                                       : 1 + elapsed * (length - 1) / 2000000;
          for (uint32_t position = 0; position < filled; ++position) {
            const uint32_t physical = !all && identify_reversed ? length - 1 - position : position;
            source.set_pixel(physical, alpha_color(color(0, 220, 233)));
          }
          if (!all || filled) {
            source.set_pixel(!all && identify_reversed ? length - 1 : 0,
                             alpha_color(color(222, 80, 233)));
            if (length > 1)
              source.set_pixel(!all && identify_reversed ? 0 : length - 1,
                               alpha_color(color(222, 80, 233)));
          }
        }
        source.show();
      };
      if (identify_driver == IDENTIFY_ALL_DRIVERS) {
        for (uint32_t id = 1; id <= connected_lighting_drivers::MAX_DRIVERS; ++id) {
          auto* driver = connected_lighting_drivers::find(id);
          if (driver && driver->_is_ready())
            for (uint32_t source = 0; source < driver->get_layout_source_count(); ++source)
              draw(driver, source, true);
        }
      } else if (auto* driver = connected_lighting_drivers::find(identify_driver)) {
        draw(driver, identify_source, false);
      }
    }
  }
};
heap_object<service> instance;
} // namespace

static bool enqueue_sensor_sample(const signal_source::sample& sample);

esp_err_t init() {
  if (instance)
    return ESP_ERR_INVALID_STATE;
  radio_queue = xQueueCreate(16, sizeof(packet));
  priority_queue = xQueueCreate(16, sizeof(packet));
  ble_queue = xQueueCreate(8, sizeof(packet));
  gpio_queue = xQueueCreate(64, sizeof(gpio_event));
  sensor_queue = xQueueCreate(32, sizeof(signal_source::sample));
  signal_queue = xQueueCreate(4, sizeof(signal_packet));
  if (!radio_queue || !priority_queue || !ble_queue || !gpio_queue || !sensor_queue ||
      !signal_queue)
    return ESP_ERR_NO_MEM;
  instance = make_heap_object<service>();
  if (!instance)
    return ESP_ERR_NO_MEM;
  if (xTaskCreatePinnedToCore([](void*) { instance->run(); }, "matrix", 12288, nullptr, 5,
                              &worker_task, portNUM_PROCESSORS - 1) != pdPASS)
    return ESP_ERR_NO_MEM;
  initialized.store(true);
  signal_source::set_sample_sink(enqueue_sensor_sample);
  return ESP_OK;
}
bool receive(const uint8_t mac[6], const uint8_t* data, size_t size) {
  if (size < 4 || matrix::wire::get(data, 4) != matrix::wire::MAGIC)
    return false;
  if (!initialized.load() || !matrix::wire::valid(data, size))
    return true;
  if (data[5] == w::SIGNAL_BATCH || data[5] == w::SENSOR_BATCH) {
    signal_packet frame{};
    memcpy(frame.mac, mac, 6);
    frame.size = size;
    memcpy(frame.data, data, size);
    if (xQueueSend(signal_queue, &frame, 0) != pdTRUE) {
      signal_packet discarded;
      xQueueReceive(signal_queue, &discarded, 0);
      xQueueSend(signal_queue, &frame, 0);
    }
    if (worker_task)
      xTaskNotifyGive(worker_task);
    return true;
  }
  packet p;
  memcpy(p.mac, mac, 6);
  memcpy(p.data, data, size);
  p.size = size;
  auto queue = data[5] == w::COMMAND || data[5] == w::COMMAND_ACK || data[5] == w::ACK ||
                       data[5] == w::GPIO_EVENT || data[5] == w::GPIO_EVENT_ACK ||
                       data[5] == w::GPIO_STATE || data[5] == w::GPIO_STATE_ACK ||
                       data[5] == w::GPIO_STATE_REQUEST || data[5] == w::IDENTIFY ||
                       data[5] == w::IDENTIFY_ACK
                   ? priority_queue
                   : radio_queue;
  if (xQueueSend(queue, &p, 0) != pdTRUE)
    overflow.store(true);
  if (worker_task)
    xTaskNotifyGive(worker_task);
  return true;
}
static bool enqueue_sensor_sample(const signal_source::sample& sample) {
  if (!initialized.load() || !sample.sensor_id || !std::isfinite(sample.value))
    return false;
  if (xQueueSend(sensor_queue, &sample, 0) != pdTRUE) {
    signal_source::sample discarded{};
    xQueueReceive(sensor_queue, &discarded, 0);
    if (xQueueSend(sensor_queue, &sample, 0) != pdTRUE)
      return false;
  }
  if (worker_task)
    xTaskNotifyGive(worker_task);
  return true;
}
bool upload(uint16_t connection, const uint8_t* data, size_t size) {
  if (!initialized.load() || size > 512 || !size)
    return false;
  packet p;
  p.connection = connection;
  p.generation = ble_generation.load();
  memcpy(p.data, data, size);
  p.size = size;
  const bool queued = xQueueSend(ble_queue, &p, 0) == pdTRUE;
  if (queued && worker_task)
    xTaskNotifyGive(worker_task);
  return queued;
}
void disconnect(uint16_t) {
  ++ble_generation;
  if (worker_task)
    xTaskNotifyGive(worker_task);
}
mesh_interop_MatrixStatus status() {
  std::lock_guard<std::mutex> lock(status_mutex);
  return published;
}
size_t read_status(uint8_t* destination, size_t capacity) {
  std::lock_guard<std::mutex> lock(status_mutex);
  if (capacity < published_size)
    return 0;
  memcpy(destination, published_bytes.data(), published_size);
  return published_size;
}
size_t read_layout_response(uint8_t* destination, size_t capacity) {
  std::lock_guard<std::mutex> lock(status_mutex);
  if (capacity < layout_response_size)
    return 0;
  memcpy(destination, layout_response_bytes.data(), layout_response_size);
  return layout_response_size;
}
void set_status_callback(void (*callback)()) { notify.store(callback); }
} // namespace matrix_service
