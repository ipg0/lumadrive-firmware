#include "matrix_service.hpp"
#include "matrix_core.hpp"
#include "matrix_wire.hpp"
#include "../connected_lighting_drivers/connected_lighting_drivers.hpp"
#include "../global_config/global_config.hpp"
#include "../master_clock_manager/master_clock_manager.hpp"
#include "../mesh_interop/mesh_interop.hpp"
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <esp_log.h>
#include <esp_now.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs.h>

namespace matrix_service {
namespace {
using namespace matrix;
namespace w = matrix::wire;
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
struct gpio_event { uint64_t at; uint32_t pin; bool rising; };
struct pending_gpio_event {
  gpio_event edge;
  uint64_t revision;
  uint32_t sequence;
};
QueueHandle_t radio_queue = nullptr, priority_queue = nullptr, ble_queue = nullptr, gpio_queue = nullptr;
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

struct peer {
  uint8_t mac[6]{};
  uint64_t seen = 0, last_retry = 0, last_recover = 0;
  bool definitions = false, ready = false, snapshotting = false;
  bool replacement_prepared = false;
  bool attempted = false;
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
  bool configured_pins[40]{};
  uint64_t gpio_stream = 0, gpio_last_send = 0;
  uint32_t gpio_sequence = 0;
  unsigned gpio_retries = 0;
  bool gpio_degraded = false;
  std::deque<pending_gpio_event> gpio_events;
  std::unique_ptr<matrix_config> config, candidate;
  std::unique_ptr<matrix_config> retiring_config;
  std::unique_ptr<player> ledger, local, pending_player;
  std::unique_ptr<player> prepared_ledger, prepared_local;
  std::unique_ptr<player> retiring_ledger;
  std::unique_ptr<animation_snapshot> pending_snapshot;
  evaluator rules;
  evaluator retiring_rules;
  receiver ble_rx, radio_rx;
  std::array<peer, 16> peers{};
  size_t peer_count = 0;
  outbound tx;
  mesh_interop_MatrixStatus state{};

  void error(const char *message, bool fatal = false) {
    snprintf(state.error, sizeof(state.error), "%s", message);
    if (fatal) state.stage = STAGE_FAILED;
    ESP_LOGW("matrix", "%s", message);
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
          published_size = bytes.size(); published = state;
        }
      }
    }
    if (changed) { auto cb = notify.load(); if (cb) cb(); }
  }
  void send(const uint8_t *dest, w::type type, const std::vector<uint8_t> &body) {
    auto p = w::packet(type, body);
    if (!p.empty()) esp_now_send(dest, p.data(), p.size());
  }
  int find_peer(const uint8_t *address) {
    for (size_t i = 0; i < peer_count; ++i) if (!memcmp(peers[i].mac, address, 6)) return i;
    if (peer_count == peers.size()) return -1;
    memcpy(peers[peer_count].mac, address, 6);
    return peer_count++;
  }
  bool available(const peer &p, uint64_t now) const {
    return p.seen && (now <= p.seen || now - p.seen < 4000000);
  }
  const char *platform_validate(const matrix_config &c, bool check_peers) {
    if (auto e = validate(c, mac)) return e;
    for (size_t i = 0; i < c.sources_count; ++i) {
      auto &s = c.sources[i];
      if (s.kind != mesh_interop_MatrixSource_Kind_GPIO_EDGE) continue;
      // Nodes run the same wiring configuration; the owner also checks on receipt.
      if (!valid_input(s.gpio)) return "GPIO is invalid or reserved by firmware";
      if (check_peers && memcmp(s.node_mac, mac, 6) &&
          !mesh_interop::instance().slave_node_exists(s.node_mac)) return "GPIO source node is not paired";
    }
    if (check_peers) for (size_t i = 0; i < c.entries_count; ++i)
      for (size_t j = 0; j < c.entries[i].targets_count; ++j) {
        auto &t = c.entries[i].targets[j];
        if (memcmp(t.node_mac, mac, 6) && !mesh_interop::instance().slave_node_exists(t.node_mac))
          return "Target node is not paired";
      }
    return nullptr;
  }
  static void gpio_isr(void *arg) {
    gpio_event e{uint64_t(esp_timer_get_time()), uint32_t(uintptr_t(arg)), false};
    e.rising = gpio_get_level(static_cast<gpio_num_t>(e.pin));
    BaseType_t wake = pdFALSE;
    if (xQueueSendFromISR(gpio_queue, &e, &wake) != pdTRUE) gpio_overflow.store(true);
    if (worker_task) vTaskNotifyGiveFromISR(worker_task, &wake);
    if (wake) portYIELD_FROM_ISR();
  }
  static bool valid_input(uint32_t pin) {
    return pin <= 39 && GPIO_IS_VALID_GPIO(static_cast<int>(pin)) &&
        pin != CONFIG_ROLE_GPIO && pin != CONFIG_DISCOVERY_GPIO &&
        !connected_lighting_drivers::uses_gpio(pin) && !(pin >= 6 && pin <= 11);
  }
  bool prepare_gpio(const animation_definitions &d) {
    if (d.gpio_inputs_count > 32) return false;
    for (size_t i = 0; i < d.gpio_inputs_count; ++i)
      if (!valid_input(d.gpio_inputs[i])) return false;
    auto err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    std::array<bool, 40> added{};
    for (size_t i = 0; i < d.gpio_inputs_count; ++i) {
      const auto pin = d.gpio_inputs[i];
      if (configured_pins[pin]) continue;
      gpio_config_t cfg{};
      cfg.pin_bit_mask = 1ULL << pin; cfg.mode = GPIO_MODE_INPUT;
      cfg.intr_type = GPIO_INTR_ANYEDGE;
      if (gpio_config(&cfg) != ESP_OK ||
          gpio_isr_handler_add(static_cast<gpio_num_t>(pin), gpio_isr,
                               reinterpret_cast<void *>(uintptr_t(pin))) != ESP_OK) {
        for (uint32_t j = 0; j < 40; ++j) if (added[j]) {
          gpio_isr_handler_remove(static_cast<gpio_num_t>(j)); configured_pins[j] = false;
        }
        return false;
      }
      configured_pins[pin] = true; added[pin] = true;
    }
    return true;
  }
  bool prepare_gpio(const matrix_config &c) {
    return prepare_gpio(definitions_for(c, mac, session, revision));
  }
  bool save(const std::vector<uint8_t> &bytes, uint64_t rev) {
    std::vector<uint8_t> record;
    w::put(record, w::MAGIC, 4); w::put(record, 1, 4); w::put(record, rev, 8);
    w::put(record, crc32(bytes.data(), bytes.size()), 4); w::put(record, bytes.size(), 4);
    record.insert(record.end(), bytes.begin(), bytes.end());
    nvs_handle_t handle;
    if (nvs_open("matrix", NVS_READWRITE, &handle) != ESP_OK) return false;
    auto err = nvs_set_blob(handle, "config", record.data(), record.size());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle); return err == ESP_OK;
  }
  bool prepare_runtime(const matrix_config &c, uint64_t rev) {
    auto next_ledger = std::make_unique<player>();
    auto next_local = std::make_unique<player>(connected_lighting_drivers::find);
    auto all = definitions_for(c, nullptr, session, rev);
    auto own = definitions_for(c, mac, session, rev);
    if (!next_ledger->prepare(all) || !next_local->prepare(own)) return false;
    prepared_ledger = std::move(next_ledger); prepared_local = std::move(next_local);
    return true;
  }
  void load() {
    nvs_handle_t handle;
    if (nvs_open("matrix", NVS_READONLY, &handle) != ESP_OK) return;
    size_t size = 0;
    auto err = nvs_get_blob(handle, "config", nullptr, &size);
    std::vector<uint8_t> record;
    if (err == ESP_OK && size >= 24 && size <= MAX_BYTES + 24) {
      record.resize(size); err = nvs_get_blob(handle, "config", record.data(), &size);
    }
    nvs_close(handle);
    if (record.empty() || err != ESP_OK || w::get(record.data(), 4) != w::MAGIC ||
        w::get(record.data() + 4, 4) != 1 || w::get(record.data() + 20, 4) != size - 24 ||
        crc32(record.data() + 24, size - 24) != w::get(record.data() + 16, 4)) {
      error("Saved matrix record is invalid", true); return;
    }
    auto c = std::make_unique<matrix_config>();
    if (!decode(mesh_interop_MatrixConfig_fields, c.get(), record.data() + 24, size - 24) ||
        platform_validate(*c, false) || !prepare_gpio(*c)) {
      error("Saved matrix cannot be restored", true); return;
    }
    revision = w::get(record.data() + 8, 8);
    if (!prepare_runtime(*c, revision)) { error("Unable to prepare saved matrix", true); return; }
    start_deployment(std::move(c));
  }
  void start_deployment(std::unique_ptr<matrix_config> c) {
    candidate = std::move(c); deployment = true; state.stage = STAGE_DISTRIBUTING;
    state.revision = revision; tx = outbound{};
    for (auto &p : peers) {
      p.attempted = false; p.replacement_prepared = false;
    }
  }
  void handle_upload(const packet &p, uint64_t now) {
    if (!master || p.generation != ble_generation.load()) return;
    mesh_interop_MatrixUpload u{};
    if (!decode(mesh_interop_MatrixUpload_fields, &u, p.data, p.size)) {
      error("Malformed upload envelope", true); return;
    }
    if (!ble_rx.id() && u.which_operation == mesh_interop_MatrixUpload_abort_tag &&
        u.operation.abort == state.transfer_id) return;
    if (!ble_rx.id() && u.which_operation == mesh_interop_MatrixUpload_commit_tag &&
        u.operation.commit == state.transfer_id && (deployment || pending_snapshot || state.stage == STAGE_ACTIVE)) return;
    if (u.which_operation == mesh_interop_MatrixUpload_begin_tag) {
      if (deployment || pending_snapshot) { error("Another deployment is in progress", true); return; }
      if (ble_rx.id() && (upload_connection != p.connection || upload_generation != p.generation)) {
        error("Upload is owned by another connection", true); return;
      }
      const auto &b = u.operation.begin;
      if (!ble_rx.begin(b.transfer_id, b.total_size, b.crc32, now)) {
        error("Invalid transfer size or conflicting transfer metadata", true); return;
      }
      upload_connection = p.connection; upload_generation = p.generation;
      state.transfer_id = b.transfer_id; state.received_bytes = ble_rx.offset();
      state.stage = STAGE_RECEIVING; state.error[0] = 0;
      return;
    }
    if (p.connection != upload_connection || p.generation != upload_generation || !ble_rx.id()) {
      error("No matching upload session", true); return;
    }
    if (u.which_operation == mesh_interop_MatrixUpload_chunk_tag) {
      auto &c = u.operation.chunk;
      if (!ble_rx.append(c.transfer_id, c.offset, c.data.bytes, c.data.size, now))
        error("Invalid upload offset or chunk", true);
      else state.received_bytes = ble_rx.offset();
    } else if (u.which_operation == mesh_interop_MatrixUpload_abort_tag) {
      if (u.operation.abort == ble_rx.id()) { ble_rx.reset(); state.stage = config ? STAGE_ACTIVE : mesh_interop_MatrixStatus_Stage_IDLE; }
    } else if (u.which_operation == mesh_interop_MatrixUpload_commit_tag) {
      if (!ble_rx.complete(u.operation.commit)) { error("Upload is incomplete or CRC32 failed", true); return; }
      state.stage = mesh_interop_MatrixStatus_Stage_VALIDATING;
      auto c = std::make_unique<matrix_config>();
      if (!decode(mesh_interop_MatrixConfig_fields, c.get(), ble_rx.bytes().data(), ble_rx.bytes().size())) {
        error("Invalid matrix protobuf", true); ble_rx.reset(); return;
      }
      if (auto e = platform_validate(*c, true)) { error(e, true); ble_rx.reset(); return; }
      if (revision == UINT64_MAX || !prepare_runtime(*c, revision + 1)) {
        error("Unable to prepare replacement runtime", true); ble_rx.reset(); return;
      }
      if (!prepare_gpio(*c)) { error("Unable to configure matrix GPIOs", true); ble_rx.reset(); return; }
      if (revision == UINT64_MAX || !save(ble_rx.bytes(), revision + 1)) {
        prepared_ledger.reset(); prepared_local.reset();
        cleanup_gpio();
        error("Unable to persist matrix", true); ble_rx.reset(); return;
      }
      ++revision; ble_rx.reset(); start_deployment(std::move(c));
    } else error("Unknown upload operation", true);
  }
  void begin_transfer(int index, uint8_t kind, std::vector<uint8_t> bytes) {
    tx = outbound{}; tx.peer_index = index; tx.kind = kind; tx.bytes = std::move(bytes);
    tx.id = esp_random(); if (!tx.id) tx.id = 1;
    tx.checksum = crc32(tx.bytes.data(), tx.bytes.size());
    peers[index].snapshotting = kind == 2;
    peers[index].last_recover = esp_timer_get_time();
  }
  void transfer_step(uint64_t now) {
    if (tx.peer_index < 0 || (tx.last_send && now - tx.last_send < 100000)) return;
    auto &p = peers[tx.peer_index];
    if (tx.retries++ >= 4 || tx.bytes.empty()) {
      p.ready = false; p.snapshotting = false; p.attempted = true; p.commands.clear();
      error("Node transfer timed out"); tx = outbound{}; return;
    }
    std::vector<uint8_t> b; w::put(b, tx.id, 4);
    if (tx.phase == 0) {
      w::put(b, tx.kind, 1); w::put(b, tx.bytes.size(), 4); w::put(b, tx.checksum, 4);
      w::put(b, revision, 8); w::put(b, session, 8);
      send(p.mac, w::BEGIN, b);
    } else if (tx.phase == 1) {
      w::put(b, tx.offset, 4);
      tx.sent_end = std::min(tx.offset + w::CHUNK_SIZE, tx.bytes.size());
      b.insert(b.end(), tx.bytes.begin() + tx.offset, tx.bytes.begin() + tx.sent_end);
      send(p.mac, w::CHUNK, b);
    } else send(p.mac, w::FINISH, b);
    tx.last_send = now;
  }
  void transfer_ack(int index, const uint8_t *b, size_t n) {
    if (n != 10 || tx.peer_index != index || w::get(b, 4) != tx.id || b[8] != tx.phase) return;
    auto &p = peers[index];
    if (b[9] != 0) {
      p.ready = false; p.snapshotting = false; p.attempted = true;
      tx = outbound{}; error("Slave rejected transferred data"); return;
    }
    size_t offset = w::get(b + 4, 4);
    if (tx.phase == 0) {
      if (offset > tx.bytes.size()) return;
      tx.offset = offset; tx.phase = offset == tx.bytes.size() ? 2 : 1;
    } else if (tx.phase == 1) {
      if (offset > tx.sent_end) return;
      tx.offset = offset; tx.phase = offset == tx.bytes.size() ? 2 : 1;
    } else {
      if (deployment && tx.kind == 1) p.replacement_prepared = true;
      else p.definitions = true;
      p.last_recover = 0;
      p.attempted = true;
      if (tx.kind == 2) { p.ready = true; p.snapshotting = false; }
      tx = outbound{}; return;
    }
    tx.last_send = 0; tx.retries = 0;
  }
  void acknowledge(const uint8_t *dest, uint32_t id, uint8_t phase, bool ok) {
    std::vector<uint8_t> b; w::put(b, id, 4); w::put(b, radio_rx.offset(), 4);
    w::put(b, phase, 1); w::put(b, ok ? 0 : 1, 1); send(dest, w::ACK, b);
  }
  bool install_received() {
    if (inbound_kind == 1) {
      auto d = std::make_unique<animation_definitions>();
      if (!decode(mesh_interop_AnimationDefinitions_fields, d.get(), radio_rx.bytes().data(), radio_rx.bytes().size()) ||
          d->master_session != session || d->revision != inbound_revision) return false;
      if (local && d->revision < local->definitions().revision) return false;
      if (pending_player && d->revision < pending_player->definitions().revision) return false;
      auto p = std::make_unique<player>(connected_lighting_drivers::find);
      if (!p->prepare(*d)) return false;
      const auto *existing = pending_player ? &pending_player->definitions() : local ? &local->definitions() : nullptr;
      if (existing && existing->revision == d->revision)
        return encode(mesh_interop_AnimationDefinitions_fields, existing) == radio_rx.bytes();
      if (!prepare_gpio(*d)) return false;
      pending_player = std::move(p); pending_snapshot.reset(); cleanup_gpio(true); return true;
    }
    auto s = std::make_unique<animation_snapshot>();
    if (!decode(mesh_interop_AnimationSnapshot_fields, s.get(), radio_rx.bytes().data(), radio_rx.bytes().size()) ||
        s->master_session != session || s->revision != inbound_revision) return false;
    if (local && s->revision == local->definitions().revision && s->sequence < local_sequence) return false;
    if (pending_snapshot && pending_player && s->revision == pending_player->definitions().revision &&
        s->sequence < local_sequence) return false;
    const animation_definitions *d = pending_player ? &pending_player->definitions() : local ? &local->definitions() : nullptr;
    if (!d || d->revision != s->revision) return false;
    // Prepare alongside the active player: malformed recovery never changes it.
    auto p = std::make_unique<player>(connected_lighting_drivers::find);
    if (!p->prepare(*d) || !p->restore(*s)) return false;
    if (local && !pending_snapshot && s->revision != local->definitions().revision)
      previous_sequence = local_sequence;
    pending_player = std::move(p); local_sequence = s->sequence;
    pending_snapshot = std::move(s); return true;
  }
  bool accept_gpio_stream(peer &p, uint64_t stream) {
    if (!stream) return false;
    if (p.gpio_stream == stream) return true;
    for (auto retired : p.retired_gpio_streams) if (retired == stream) return false;
    p.retired_gpio_streams[p.retired_gpio_index++ % p.retired_gpio_streams.size()] = p.gpio_stream;
    p.gpio_stream = stream; p.gpio_sequence = 0; p.gpio_degraded = false;
    return true;
  }
  bool evaluate_gpio(const uint8_t *source_mac, const gpio_event &e, uint64_t rev, uint64_t clock) {
    const matrix_config *c = nullptr;
    evaluator *evaluation = nullptr;
    if (ledger && ledger->definitions().revision == rev &&
        (!retiring_config || e.at >= activate_at)) {
      c = config.get(); evaluation = &rules;
    } else if (retiring_ledger && retiring_ledger->definitions().revision == rev && e.at < activate_at) {
      c = retiring_config.get(); evaluation = &retiring_rules;
    }
    if (!c) return false;
    bool allowed = false;
    for (size_t i = 0; i < c->sources_count; ++i) {
      const auto &s = c->sources[i];
      if (s.kind == mesh_interop_MatrixSource_Kind_GPIO_EDGE && s.gpio == e.pin &&
          !memcmp(s.node_mac, source_mac, 6)) allowed = true;
    }
    if (!allowed) return false;
    trigger_gpio_edge_t edge{}; memcpy(edge.from_node_mac, source_mac, 6);
    edge.gpio_num = static_cast<gpio_num_t>(e.pin); edge.rising = e.rising;
    evaluation->tick(clock, &edge, 1, e.at, emit, this);
    return true;
  }
  void handle_gpio_event(peer &p, const uint8_t *b, size_t n, uint64_t now) {
    // The sender MAC comes from ESP-NOW, never from an event-supplied identity.
    if (n != 38 || w::get(b, 8) != session || !w::get(b + 24, 4) || b[37] > 1) return;
    const auto rev = w::get(b + 8, 8), stream = w::get(b + 16, 8);
    const uint32_t sequence = w::get(b + 24, 4);
    if (!accept_gpio_stream(p, stream)) return;
    bool ok = true;
    if (sequence > p.gpio_sequence) {
      if (sequence != p.gpio_sequence + 1) {
        p.gpio_degraded = true; error("Slave GPIO events were lost");
      }
      gpio_event e{w::get(b + 28, 8), b[36], b[37] != 0};
      const auto clock = uint64_t(master_clock_manager::instance().get_master_time());
      // Bound stale/future input without replacing the original event timestamp.
      const bool superseded = ledger && rev < ledger->definitions().revision &&
          (!retiring_ledger || rev != retiring_ledger->definitions().revision);
      ok = superseded || (e.at <= clock + LEAD_US &&
          (e.at >= clock || clock - e.at <= TRANSFER_TIMEOUT_US) && evaluate_gpio(p.mac, e, rev, clock));
      p.gpio_sequence = sequence;
      if (!ok) { p.gpio_degraded = true; error("Slave GPIO event rejected (pin, revision, or time)"); }
    }
    p.seen = now;
    std::vector<uint8_t> reply; w::put(reply, session, 8); w::put(reply, rev, 8);
    w::put(reply, stream, 8); w::put(reply, sequence, 4); w::put(reply, ok ? 0 : 1, 1);
    send(p.mac, w::GPIO_EVENT_ACK, reply);
  }
  void handle_radio(const packet &packet, uint64_t now) {
    if (!w::valid(packet.data, packet.size)) return;
    const auto type = static_cast<w::type>(packet.data[5]);
    const auto *b = packet.data + w::HEADER_SIZE;
    const size_t n = packet.size - w::HEADER_SIZE;
    if (master) {
      if (!mesh_interop::instance().slave_node_exists(packet.mac)) return;
      int index = find_peer(packet.mac); if (index < 0) return;
      auto &p = peers[index];
      if (type == w::GPIO_EVENT) handle_gpio_event(p, b, n, now);
      else if (type == w::ACK) transfer_ack(index, b, n);
      else if (type == w::COMMAND_ACK && n == 20 && w::get(b, 8) == session) {
        if (!p.retiring_commands.empty() && w::get(b + 8, 8) == p.retiring_commands.front().revision &&
            w::get(b + 16, 4) == p.retiring_commands.front().sequence) {
          p.retiring_commands.pop_front(); p.retiring_retries = 0; p.retiring_retry = 0;
        } else if (!p.commands.empty() && w::get(b + 8, 8) == p.commands.front().revision &&
                   w::get(b + 16, 4) == p.commands.front().sequence) {
          p.commands.pop_front(); p.retries = 0; p.last_retry = 0;
        }
      } else if (type == w::HELLO && (n == 20 || n == 29) && w::get(b, 8) == session) {
        if (n == 29) {
          if (!accept_gpio_stream(p, w::get(b + 20, 8))) return;
          if (b[28]) { p.gpio_degraded = true; error("Slave reported GPIO event loss"); }
        }
        p.seen = now;
        const auto running_revision = ledger ? ledger->definitions().revision : revision;
        if (p.ready && (w::get(b + 8, 8) != running_revision ||
            w::get(b + 16, 4) > p.sequence ||
            (p.commands.empty() && w::get(b + 16, 4) != p.sequence))) {
          p.ready = false; p.definitions = false; p.commands.clear();
        }
      } else if (type == w::SNAPSHOT_REQUEST) {
        p.ready = false; p.snapshotting = false; p.commands.clear();
        if (tx.peer_index == index && tx.kind == 2) tx = outbound{};
        p.last_recover = 0;
      }
      return;
    }
    if (!mesh_interop::instance().master_node_matches(packet.mac) ||
        !master_clock_manager::instance().is_synchronized()) return;
    if (type == w::GPIO_EVENT_ACK && n == 29 && !gpio_events.empty() &&
        w::get(b, 8) == session && w::get(b + 16, 8) == gpio_stream &&
        w::get(b + 8, 8) == gpio_events.front().revision &&
        w::get(b + 24, 4) == gpio_events.front().sequence) {
      if (b[28]) { gpio_degraded = true; error("Master rejected GPIO event"); last_hello = 0; }
      gpio_events.pop_front(); gpio_last_send = 0; gpio_retries = 0;
    } else if (type == w::BEGIN && n == 29) {
      const auto id = uint32_t(w::get(b, 4));
      bool ok = (b[4] == 1 || b[4] == 2) && w::get(b + 21, 8) == session;
      if (radio_rx.id() == id && (inbound_kind != b[4] || inbound_revision != w::get(b + 13, 8))) ok = false;
      if (local && w::get(b + 13, 8) < local->definitions().revision) ok = false;
      if (pending_player && w::get(b + 13, 8) < pending_player->definitions().revision) ok = false;
      if (ok) ok = radio_rx.begin(id, w::get(b + 5, 4), w::get(b + 9, 4), now);
      if (ok) { inbound_kind = b[4]; inbound_revision = w::get(b + 13, 8); }
      acknowledge(packet.mac, id, 0, ok);
    } else if (type == w::CHUNK && n > 8 && n <= 208) {
      uint32_t id = w::get(b, 4);
      bool ok = radio_rx.append(id, w::get(b + 4, 4), b + 8, n - 8, now);
      // A missing/out-of-order chunk gets an ACK of the expected offset.
      acknowledge(packet.mac, id, 1, ok || id == radio_rx.id());
    } else if (type == w::FINISH && n == 4) {
      uint32_t id = w::get(b, 4);
      bool ok = id == completed_transfer || (radio_rx.complete(id) && install_received());
      if (ok) completed_transfer = id;
      acknowledge(packet.mac, id, 2, ok);
    } else if (type == w::COMMAND) {
      animation_command c{};
      if (!decode(mesh_interop_AnimationCommand_fields, &c, b, n) || c.master_session != session) return;
      const bool previous = local && pending_snapshot && pending_player &&
          c.revision == local->definitions().revision && c.revision != pending_player->definitions().revision;
      auto *p = previous ? local.get() : pending_snapshot && pending_player ? pending_player.get() : local.get();
      if (!p || c.revision != p->definitions().revision) return;
      auto &sequence = previous ? previous_sequence : local_sequence;
      if (c.sequence == sequence + 1 && p->enqueue(c)) ++sequence;
      else if (c.sequence > sequence) { send(packet.mac, w::SNAPSHOT_REQUEST, {}); return; }
      std::vector<uint8_t> reply; w::put(reply, session, 8); w::put(reply, c.revision, 8);
      w::put(reply, c.sequence, 4); send(packet.mac, w::COMMAND_ACK, reply);
    }
  }
  static bool emit(void *arg, const animation_command &command) {
    auto &s = *static_cast<service *>(arg);
    const bool previous = s.retiring_ledger && command.revision == s.retiring_ledger->definitions().revision;
    if (previous && command.effective_at_us >= s.activate_at) return true;
    auto *timeline = previous ? s.retiring_ledger.get() : s.ledger.get();
    auto *config = previous ? s.retiring_config.get() : s.config.get();
    if (!timeline || !timeline->enqueue(command)) { s.error("Animation timeline queue overflow"); return false; }
    const mesh_interop_MatrixEntry *entry = nullptr;
    for (size_t i = 0; i < config->entries_count; ++i)
      if (config->entries[i].id == command.entry_id) entry = &config->entries[i];
    if (!entry) return false;
    for (size_t j = 0; j < entry->targets_count; ++j) {
      auto &target = entry->targets[j];
      bool handled = false;
      for (size_t k = 0; k < j; ++k)
        if (!memcmp(target.node_mac, entry->targets[k].node_mac, 6)) handled = true;
      if (handled) continue; // One command drives all this entry's local outputs.
      if (!memcmp(target.node_mac, s.mac, 6)) {
        auto *p = previous ? s.local.get() : s.pending_snapshot ? s.pending_player.get() : s.local.get();
        if (p && !p->enqueue(command)) s.error("Local animation queue overflow");
      } else {
        int index = s.find_peer(target.node_mac); if (index < 0) continue;
        auto &p = s.peers[index];
        if (previous) {
          if (p.retiring_commands.size() < 64) {
            auto c = command; c.sequence = ++p.retiring_sequence; p.retiring_commands.push_back(c);
          } else s.error("Retiring command queue overflow");
          continue;
        }
        if (!p.ready && !p.snapshotting) continue;
        if (p.commands.size() == 64 || p.sequence == UINT32_MAX) {
          p.ready = false; p.snapshotting = false; p.commands.clear();
          if (s.tx.peer_index == index && s.tx.kind == 2) s.tx = outbound{};
          p.last_recover = 0;
          s.error("Node command queue overflow; recovering snapshot"); continue;
        }
        auto c = command; c.sequence = ++p.sequence; p.commands.push_back(c);
      }
    }
    return true;
  }
  void activate(uint64_t now) {
    if (!prepared_ledger || !prepared_local) {
      error("Unable to allocate replacement animation runtime", true); deployment = false; return;
    }
    retiring_config = std::move(config); retiring_ledger = std::move(ledger); retiring_rules = rules;
    config = std::move(candidate); ledger = std::move(prepared_ledger);
    for (auto &p : peers) {
      p.retiring_commands = std::move(p.commands); p.retiring_sequence = p.sequence;
      p.retiring_retry = p.last_retry; p.retiring_retries = p.retries;
      p.ready = false; p.definitions = p.replacement_prepared;
      p.snapshotting = false; p.commands.clear(); p.sequence = 0;
      p.last_retry = 0; p.retries = 0;
      p.last_recover = 0;
    }
    pending_player = std::move(prepared_local); pending_snapshot = std::make_unique<animation_snapshot>();
    // Configuration rollout can take longer than one trigger's 100 ms lead.
    // Reserve a shared epoch far enough ahead to transfer initial snapshots.
    activate_at = now + LEAD_US + peer_count * 500000;
    pending_snapshot->activate_at_us = activate_at;
    rules.configure(config.get(), session, revision);
    rules.tick(activate_at - LEAD_US, nullptr, 0, now, emit, this);
    deployment = false;
  }
  void cleanup_gpio(bool keep_pending = false) {
    // Retire old handlers only at the shared activation boundary.
    for (uint32_t pin = 0; pin < 40; ++pin) {
      bool used = false;
      for (const auto *p : {local.get(), keep_pending ? pending_player.get() : nullptr}) {
        if (!p) continue;
        const auto &d = p->definitions();
        for (size_t i = 0; i < d.gpio_inputs_count; ++i)
          if (d.gpio_inputs[i] == pin) used = true;
      }
      if (configured_pins[pin] && !used) {
        gpio_isr_handler_remove(static_cast<gpio_num_t>(pin)); configured_pins[pin] = false;
      }
    }
  }
  void collect_gpio(uint64_t clock) {
    if (gpio_overflow.exchange(false)) {
      gpio_degraded = true; last_hello = 0; error("GPIO capture queue overflow; edges lost");
    }
    gpio_event e;
    for (unsigned i = 0; i < 64 && xQueueReceive(gpio_queue, &e, 0) == pdTRUE; ++i) {
      // ISR timestamps are local monotonic time. Apply the current synchronized
      // offset in task context, retaining queue latency rather than using arrival time.
      if (e.at > uint64_t(esp_timer_get_time())) continue;
      const auto timestamp = master_clock_manager::instance().get_master_time_at(e.at);
      if (timestamp < 0) continue;
      e.at = uint64_t(timestamp);
      const auto *p = pending_snapshot && pending_player && e.at >= pending_snapshot->activate_at_us ?
          pending_player.get() : local.get();
      if (!p) continue;
      const auto &d = p->definitions();
      if (std::find(d.gpio_inputs, d.gpio_inputs + d.gpio_inputs_count, e.pin) ==
          d.gpio_inputs + d.gpio_inputs_count) continue;
      if (master) evaluate_gpio(mac, e, d.revision, clock);
      else if (gpio_events.size() < 64 && gpio_sequence != UINT32_MAX) {
        gpio_events.push_back({e, d.revision, ++gpio_sequence});
      } else {
        gpio_degraded = true; last_hello = 0; error("GPIO delivery queue overflow; edges lost");
      }
    }
  }
  void gpio_send_step(const uint8_t *dest, uint64_t now) {
    if (gpio_events.empty() || (gpio_last_send && now - gpio_last_send < 100000)) return;
    if (gpio_retries++ >= 4) {
      gpio_events.pop_front(); gpio_retries = 0; gpio_last_send = 0;
      gpio_degraded = true; last_hello = 0; error("GPIO event timed out; edge lost"); return;
    }
    const auto &e = gpio_events.front();
    std::vector<uint8_t> b; w::put(b, session, 8); w::put(b, e.revision, 8);
    w::put(b, gpio_stream, 8); w::put(b, e.sequence, 4); w::put(b, e.edge.at, 8);
    w::put(b, e.edge.pin, 1); w::put(b, e.edge.rising, 1);
    send(dest, w::GPIO_EVENT, b); gpio_last_send = now;
  }
  void master_step(uint64_t now, uint64_t clock) {
    auto mesh = mesh_interop::instance().get_state();
    for (size_t i = 0; i < mesh.nodes_count; ++i) {
      int idx = find_peer(mesh.nodes[i].mac);
      if (idx >= 0 && mesh.nodes[i].last_synced_at)
        peers[idx].seen = std::max(peers[idx].seen, mesh.nodes[i].last_synced_at);
    }
    if (deployment && tx.peer_index < 0) {
      bool started = false;
      for (size_t i = 0; i < peer_count; ++i) {
        auto &p = peers[i];
        if (p.attempted || !available(p, now)) continue;
        auto d = definitions_for(*candidate, p.mac, session, revision);
        begin_transfer(i, 1, encode(mesh_interop_AnimationDefinitions_fields, &d));
        started = true; break;
      }
      if (!started) activate(clock);
    }
    collect_gpio(clock);
    if (config && ledger) {
      if (retiring_config && clock < activate_at) {
        retiring_rules.tick(clock, nullptr, 0, clock, emit, this);
        retiring_ledger->tick(clock, false);
      }
      if (clock >= activate_at) rules.tick(clock, nullptr, 0, clock, emit, this);
      ledger->tick(clock, false);
      for (size_t i = 0; i < peer_count; ++i) {
        auto &p = peers[i];
        if (!available(p, now)) { p.ready = false; p.commands.clear(); continue; }
        if (clock < activate_at && !p.retiring_commands.empty() &&
            (!p.retiring_retry || now - p.retiring_retry >= 100000)) {
          if (p.retiring_retries++ < 4) {
            send(p.mac, w::COMMAND, encode(mesh_interop_AnimationCommand_fields, &p.retiring_commands.front()));
            p.retiring_retry = now;
          } else p.retiring_commands.clear();
        }
        if (p.ready && !p.commands.empty() && (!p.last_retry || now - p.last_retry >= 100000)) {
          if (p.retries++ >= 4) {
            p.ready = false; p.commands.clear(); error("Animation command timed out; recovering snapshot");
          } else {
            send(p.mac, w::COMMAND, encode(mesh_interop_AnimationCommand_fields, &p.commands.front()));
            p.last_retry = now;
          }
        }
        if (!deployment && !p.ready && !p.snapshotting && tx.peer_index < 0 && now - p.last_recover >= 1000000) {
          auto d = definitions_for(*config, p.mac, session, revision);
          if (!p.definitions) begin_transfer(i, 1, encode(mesh_interop_AnimationDefinitions_fields, &d));
          else {
            p.commands.clear(); p.retries = 0; p.last_retry = 0;
            auto snapshot = std::make_unique<animation_snapshot>(ledger->snapshot(d, p.sequence, activate_at));
            begin_transfer(i, 2, encode(mesh_interop_AnimationSnapshot_fields, snapshot.get()));
          }
        }
      }
    }
    state.nodes_count = 0;
    auto &self = state.nodes[state.nodes_count++]; memcpy(self.node_mac, mac, 6);
    self.state = gpio_degraded ? NODE_DEGRADED : NODE_READY;
    for (size_t i = 0; i < peer_count; ++i) {
      auto &s = state.nodes[state.nodes_count++]; auto &p = peers[i]; memcpy(s.node_mac, p.mac, 6);
      s.state = !available(p, now) ? NODE_UNAVAILABLE : p.ready ? (p.gpio_degraded ? NODE_DEGRADED : NODE_READY) :
          (tx.peer_index == int(i) || deployment) ? NODE_PREPARING : NODE_DEGRADED;
    }
  }
  void slave_step(uint64_t now) {
    auto &clock = master_clock_manager::instance();
    if (!clock.is_synchronized()) {
      gpio_event e;
      for (unsigned i = 0; i < 64 && xQueueReceive(gpio_queue, &e, 0) == pdTRUE; ++i) {}
      return;
    }
    const auto observed = clock.session();
    if (session != observed) {
      if (local) local->blank();
      session = observed; local.reset(); pending_player.reset(); pending_snapshot.reset();
      radio_rx.reset(); completed_transfer = 0; local_sequence = 0; revision = 0;
      gpio_events.clear(); gpio_sequence = 0; gpio_stream = 0;
      gpio_retries = 0; gpio_last_send = 0; gpio_degraded = false; last_hello = 0;
      cleanup_gpio();
      gpio_event e;
      for (unsigned i = 0; i < 64 && xQueueReceive(gpio_queue, &e, 0) == pdTRUE; ++i) {}
    }
    if (!gpio_stream) {
      gpio_stream = (uint64_t(esp_random()) << 32) | esp_random();
      if (!gpio_stream) gpio_stream = 1;
    }
    collect_gpio(uint64_t(clock.get_master_time()));
    auto mesh = mesh_interop::instance().get_state();
    if (mesh.nodes_count) gpio_send_step(mesh.nodes[0].mac, now);
    if (now - last_hello >= 1000000) {
      if (mesh.nodes_count) {
        std::vector<uint8_t> b; w::put(b, session, 8);
        auto *advertised = pending_snapshot && pending_player ? pending_player.get() : local.get();
        w::put(b, advertised ? advertised->definitions().revision : 0, 8); w::put(b, local_sequence, 4);
        w::put(b, gpio_stream, 8); w::put(b, gpio_degraded ? 1 : 0, 1);
        send(mesh.nodes[0].mac, w::HELLO, b);
      }
      last_hello = now;
    }
  }
  void run() {
    master = global_config::instance().is_master_node();
    memcpy(mac, global_config::instance().get_mesh_mac(), 6);
    session = master_clock_manager::instance().session(); state.master_session = session;
    memcpy(state.master_mac, mac, 6);
    if (master) load();
    TickType_t next_frame = xTaskGetTickCount();
    for (;;) {
      uint64_t now = esp_timer_get_time();
      if (!master) slave_step(now);
      packet p;
      for (unsigned i = 0; i < 16 && xQueueReceive(priority_queue, &p, 0) == pdTRUE; ++i) handle_radio(p, now);
      for (unsigned i = 0; i < 8 && xQueueReceive(radio_queue, &p, 0) == pdTRUE; ++i) handle_radio(p, now);
      if (ble_rx.id() && upload_generation != ble_generation.load()) {
        ble_rx.reset(); error("Upload connection closed", true);
      }
      if (xQueueReceive(ble_queue, &p, 0) == pdTRUE) handle_upload(p, now);
      uint32_t old_id = ble_rx.id(); ble_rx.expire(now); radio_rx.expire(now);
      if (old_id && !ble_rx.id()) error("Upload timed out", true);
      auto clock = static_cast<uint64_t>(master_clock_manager::instance().get_master_time());
      if (master) master_step(now, clock);
      transfer_step(now);
      const TickType_t ticks = xTaskGetTickCount();
      if (static_cast<int32_t>(ticks - next_frame) >= 0) {
        playback_step(clock);
        next_frame = ticks + pdMS_TO_TICKS(10);
      }
      if (overflow.exchange(false)) {
        error("Transport queue overflow");
        if (!master) {
          auto mesh = mesh_interop::instance().get_state();
          if (mesh.nodes_count) send(mesh.nodes[0].mac, w::SNAPSHOT_REQUEST, {});
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
  void playback_step(uint64_t clock) {
    if (pending_snapshot && pending_player && clock >= pending_snapshot->activate_at_us) {
      if (local) local->blank();
      local = std::move(pending_player); revision = local->definitions().revision;
      pending_snapshot.reset();
      retiring_config.reset(); retiring_ledger.reset();
      for (auto &p : peers) p.retiring_commands.clear();
      cleanup_gpio();
      if (master) state.stage = STAGE_ACTIVE;
    }
    if (local && master_clock_manager::instance().is_synchronized()) {
      auto err = local->tick(clock);
      if (err != ESP_OK && !state.error[0]) error("Animation driver failed");
    }
  }
};
service *instance = nullptr;
} // namespace

esp_err_t init() {
  if (instance) return ESP_ERR_INVALID_STATE;
  radio_queue = xQueueCreate(16, sizeof(packet)); priority_queue = xQueueCreate(16, sizeof(packet));
  ble_queue = xQueueCreate(8, sizeof(packet)); gpio_queue = xQueueCreate(64, sizeof(gpio_event));
  if (!radio_queue || !priority_queue || !ble_queue || !gpio_queue) return ESP_ERR_NO_MEM;
  instance = new (std::nothrow) service;
  if (!instance) return ESP_ERR_NO_MEM;
  if (xTaskCreate([](void *) { instance->run(); }, "matrix", 12288, nullptr, 5, &worker_task) != pdPASS)
    return ESP_ERR_NO_MEM;
  initialized.store(true);
  return ESP_OK;
}
bool receive(const uint8_t mac[6], const uint8_t *data, size_t size) {
  if (size < 4 || matrix::wire::get(data, 4) != matrix::wire::MAGIC) return false;
  if (!initialized.load() || !matrix::wire::valid(data, size)) return true;
  packet p; memcpy(p.mac, mac, 6); memcpy(p.data, data, size); p.size = size;
  auto queue = data[5] == w::COMMAND || data[5] == w::COMMAND_ACK || data[5] == w::ACK ||
      data[5] == w::GPIO_EVENT || data[5] == w::GPIO_EVENT_ACK ? priority_queue : radio_queue;
  if (xQueueSend(queue, &p, 0) != pdTRUE) overflow.store(true);
  if (worker_task) xTaskNotifyGive(worker_task);
  return true;
}
bool upload(uint16_t connection, const uint8_t *data, size_t size) {
  if (!initialized.load() || size > 512 || !size) return false;
  packet p; p.connection = connection; p.generation = ble_generation.load();
  memcpy(p.data, data, size); p.size = size;
  const bool queued = xQueueSend(ble_queue, &p, 0) == pdTRUE;
  if (queued && worker_task) xTaskNotifyGive(worker_task);
  return queued;
}
void disconnect(uint16_t) {
  ++ble_generation;
  if (worker_task) xTaskNotifyGive(worker_task);
}
mesh_interop_MatrixStatus status() {
  std::lock_guard<std::mutex> lock(status_mutex); return published;
}
size_t read_status(uint8_t *destination, size_t capacity) {
  std::lock_guard<std::mutex> lock(status_mutex);
  if (capacity < published_size) return 0;
  memcpy(destination, published_bytes.data(), published_size); return published_size;
}
void set_status_callback(void (*callback)()) { notify.store(callback); }
} // namespace matrix_service
