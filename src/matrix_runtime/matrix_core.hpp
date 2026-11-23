#pragma once

#include "../animation_trigger_table/animation_effect/animation_effect.hpp"
#include "../animation_trigger_table/trigger_context.hpp"
#include "heap_object.hpp"
#include <array>
#include <memory>
#include <vector>
#include <mesh.pb.h>

namespace matrix {
constexpr size_t MAX_BYTES = 16384;
constexpr size_t MAX_DRIVERS_PER_NODE = 6;
constexpr size_t MAX_SOURCES_PER_NODE = 12;
constexpr size_t MAX_LAYOUT_SOURCES = 84;
constexpr size_t MAX_INVENTORY_SOURCES = 204;
constexpr uint32_t SCHEMA_VERSION = 0;
constexpr uint64_t LEAD_US = 100000;
constexpr uint64_t TRANSFER_TIMEOUT_US = 5000000;
using matrix_config = mesh_interop_MatrixConfig;
using animation_definitions = mesh_interop_AnimationDefinitions;
using animation_command = mesh_interop_AnimationCommand;
using animation_snapshot = mesh_interop_AnimationSnapshot;
using matrix_effect = mesh_interop_MatrixEffect;
using output_mapping = mesh_interop_AnimationOutputMapping;
using driver_resolver = rgb_lighting_driver* (*)(uint32_t);

// Retain only populated definitions between uploads. A Nanopb MatrixConfig
// reserves every effect/layout slot even for a tiny matrix; use it for staging.
struct runtime_config {
  runtime_config()
      : schema_version(SCHEMA_VERSION), factory_default(false), effects_count(0), sources_count(0),
        conditions_count(0), entries_count(0), layout_count(0), modes_count(0), active_mode_id(0) {}
  explicit runtime_config(const matrix_config& config);
  void copy_to(matrix_config& config) const;
  heap_object<matrix_config> to_wire() const;
  const std::vector<uint8_t>& encoded() const { return _encoded; }
  bool reencode();
  uint32_t schema_version;
  bool factory_default;
  size_t effects_count, sources_count, conditions_count, entries_count, layout_count, modes_count;
  uint32_t active_mode_id;
  bool has_center_index = false;
  uint32_t center_index = 0;
  std::vector<matrix_effect> effects;
  std::vector<mesh_interop_MatrixSource> sources;
  std::vector<mesh_interop_MatrixCondition> conditions;
  std::vector<mesh_interop_MatrixEntry> entries;
  std::vector<mesh_interop_MatrixLayout> layout;
  std::vector<mesh_interop_MatrixMode> modes;

private:
  std::vector<uint8_t> _encoded;
};

// Keep only scheduling/rendering metadata after preparing effects. The protobuf
// has room for 32 layers with 16 keyframes each, even for a one-layer rainbow.
struct playback_layer {
  uint32_t entry_id = 0, priority = 0, order = 0;
  bool opaque_background = false;
  uint32_t region_start = 0, region_end = UINT32_MAX;
};
struct playback_definitions {
  playback_definitions() = default;
  playback_definitions(const animation_definitions& definitions);
  playback_definitions(const matrix_config& config, const uint8_t* node_mac, uint64_t session,
                       uint64_t revision);
  playback_definitions(const runtime_config& config, const uint8_t* node_mac, uint64_t session,
                       uint64_t revision);
  uint64_t master_session = 0, revision = 0;
  uint32_t canvas_size = 0;
  uint32_t center_index = 0;
  size_t layers_count = 0, output_mappings_count = 0, gpio_inputs_count = 0;
  std::array<playback_layer, 32> layers{};
  std::array<output_mapping, MAX_SOURCES_PER_NODE> output_mappings{};
  std::array<uint32_t, 32> gpio_inputs{};
};

uint32_t crc32(const uint8_t* data, size_t length);
std::vector<uint8_t> encode(const pb_msgdesc_t* fields, const void* message,
                            size_t limit = MAX_BYTES);
bool decode(const pb_msgdesc_t* fields, void* message, const uint8_t* data, size_t length);

class receiver {
public:
  bool begin(uint32_t id, size_t size, uint32_t checksum, uint64_t now);
  bool append(uint32_t id, size_t offset, const uint8_t* data, size_t size, uint64_t now);
  bool complete(uint32_t id) const;
  void expire(uint64_t now);
  void reset();
  uint32_t id() const { return _id; }
  size_t offset() const { return _offset; }
  const std::vector<uint8_t>& bytes() const { return _bytes; }

private:
  uint32_t _id = 0, _checksum = 0;
  size_t _offset = 0;
  uint64_t _touched = 0;
  std::vector<uint8_t> _bytes;
};

// Validate structural references here; platform code additionally validates
// paired MACs and GPIO capabilities. Returns nullptr on success.
const char* validate(const matrix_config& config, const uint8_t master_mac[6]);
const char* validate(const runtime_config& config, const uint8_t master_mac[6]);
animation_definitions definitions_for(const matrix_config& config, const uint8_t* node_mac,
                                      uint64_t session, uint64_t revision);
// Heap-backed to keep keyframe buffers off the worker stack; nullptr on allocation failure.
heap_object<animation_definitions> make_definitions(const matrix_config& config,
                                                    const uint8_t* node_mac, uint64_t session,
                                                    uint64_t revision);
heap_object<animation_definitions> make_definitions(const runtime_config& config,
                                                    const uint8_t* node_mac, uint64_t session,
                                                    uint64_t revision);

// One reusable instance per layer; commands and snapshots never evaluate rules.
class player {
public:
  explicit player(driver_resolver resolver = nullptr) : _resolver(resolver) {}
  bool prepare(const animation_definitions& definitions);
  bool prepare(const matrix_config& config, const uint8_t* node_mac, uint64_t session,
               uint64_t revision);
  bool prepare(const runtime_config& config, const uint8_t* node_mac, uint64_t session,
               uint64_t revision);
  bool prepare(const player& source);
  // Prepare a single temporary layer against an existing node's layout.
  bool prepare_preview(const playback_definitions& layout, const matrix_effect& effect);
  bool enqueue(const animation_command& command);
  bool restore(const animation_snapshot& snapshot);
  animation_snapshot snapshot(const playback_definitions& subset, uint32_t sequence,
                              uint64_t activate_at) const;
  esp_err_t tick(uint64_t now, bool render = true, uint32_t excluded_driver = 0,
                 uint32_t excluded_source = 0);
  esp_err_t blank();
  const playback_definitions& definitions() const { return _definitions; }
  const std::vector<uint8_t>& definition_bytes() const { return _definition_bytes; }

private:
  struct layer {
    std::unique_ptr<animation_effect_instance_base> effect;
    bool active = false;
    bool begun = false;
    uint64_t started = 0;
    uint64_t elapsed = 0;
  };
  int find(uint32_t id) const;
  bool prepare(const playback_definitions& definitions,
               const std::array<const matrix_effect*, 32>& effects);
  playback_definitions _definitions{};
  std::vector<uint8_t> _definition_bytes;
  std::array<layer, 32> _layers;
  std::array<animation_command, 64> _pending{};
  size_t _pending_count = 0;
  driver_resolver _resolver;
};

class evaluator {
public:
  using command_sink = bool (*)(void*, const animation_command&);
  void set_gpio_lookup(bool (*lookup)(void*, const uint8_t*, uint32_t, bool&), void* arg) {
    _gpio_lookup = lookup;
    _gpio_arg = arg;
  }
  template <typename config_type>
  void configure(const config_type* config, uint64_t session, uint64_t revision, bool boot = false,
                 bool mode_switched = false) {
    _entries = config ? std::data(config->entries) : nullptr;
    _sources = config ? std::data(config->sources) : nullptr;
    _effects = config ? std::data(config->effects) : nullptr;
    _conditions = config ? std::data(config->conditions) : nullptr;
    _entries_count = config ? config->entries_count : 0;
    _sources_count = config ? config->sources_count : 0;
    _effects_count = config ? config->effects_count : 0;
    _conditions_count = config ? config->conditions_count : 0;
    _mode = config ? config->active_mode_id : 0;
    _session = session;
    _revision = revision;
    _starts.fill(0);
    _active.fill(false);
    _boot = boot;
    _boot_fired.fill(false);
    _mode_switched = mode_switched;
    _mode_fired.fill(false);
  }
  void tick(uint64_t now, const trigger_gpio_edge_t* edges, size_t edge_count, uint64_t edge_time,
            command_sink sink, void* arg);

private:
  bool (*_gpio_lookup)(void*, const uint8_t*, uint32_t, bool&) = nullptr;
  void* _gpio_arg = nullptr;
  const mesh_interop_MatrixEntry* _entries = nullptr;
  const mesh_interop_MatrixSource* _sources = nullptr;
  const matrix_effect* _effects = nullptr;
  const mesh_interop_MatrixCondition* _conditions = nullptr;
  size_t _entries_count = 0, _sources_count = 0, _effects_count = 0;
  size_t _conditions_count = 0;
  uint32_t _mode = 0;
  uint64_t _session = 0, _revision = 0;
  std::array<uint64_t, 32> _starts{};
  std::array<bool, 32> _active{};
  std::array<bool, 32> _boot_fired{};
  std::array<bool, 32> _mode_fired{};
  bool _boot = false, _mode_switched = false;
};
} // namespace matrix
