#pragma once

#include "../animation_trigger_table/animation_effect/animation_effect.hpp"
#include "../animation_trigger_table/trigger_context.hpp"
#include <array>
#include <memory>
#include <vector>
#include <mesh.pb.h>

namespace matrix {
constexpr size_t MAX_BYTES = 16384;
constexpr uint32_t SCHEMA_VERSION = 2;
constexpr uint64_t LEAD_US = 100000;
constexpr uint64_t TRANSFER_TIMEOUT_US = 5000000;
using matrix_config = mesh_interop_MatrixConfig;
using animation_definitions = mesh_interop_AnimationDefinitions;
using animation_command = mesh_interop_AnimationCommand;
using animation_snapshot = mesh_interop_AnimationSnapshot;
using matrix_effect = mesh_interop_MatrixEffect;
using output_mapping = mesh_interop_AnimationOutputMapping;
using driver_resolver = rgb_lighting_driver *(*)(uint32_t);

uint32_t crc32(const uint8_t *data, size_t length);
std::vector<uint8_t> encode(const pb_msgdesc_t *fields, const void *message);
bool decode(const pb_msgdesc_t *fields, void *message,
            const uint8_t *data, size_t length);

class receiver {
public:
  bool begin(uint32_t id, size_t size, uint32_t checksum, uint64_t now);
  bool append(uint32_t id, size_t offset, const uint8_t *data, size_t size,
              uint64_t now);
  bool complete(uint32_t id) const;
  void expire(uint64_t now);
  void reset();
  uint32_t id() const { return _id; }
  size_t offset() const { return _offset; }
  const std::vector<uint8_t> &bytes() const { return _bytes; }
private:
  uint32_t _id = 0, _checksum = 0;
  size_t _offset = 0;
  uint64_t _touched = 0;
  std::vector<uint8_t> _bytes;
};

// Validate structural references here; platform code additionally validates
// paired MACs and GPIO capabilities. Returns nullptr on success.
const char *validate(const matrix_config &config, const uint8_t master_mac[6]);
animation_definitions definitions_for(const matrix_config &config, const uint8_t *node_mac,
                            uint64_t session, uint64_t revision);
// Heap-backed to keep keyframe buffers off the worker stack; nullptr on allocation failure.
std::unique_ptr<animation_definitions> make_definitions(const matrix_config &config,
    const uint8_t *node_mac, uint64_t session, uint64_t revision);

// One reusable instance per layer; commands and snapshots never evaluate rules.
class player {
public:
  explicit player(driver_resolver resolver = nullptr) : _resolver(resolver) {}
  bool prepare(const animation_definitions &definitions);
  bool enqueue(const animation_command &command);
  bool restore(const animation_snapshot &snapshot);
  animation_snapshot snapshot(const animation_definitions &subset, uint32_t sequence,
                    uint64_t activate_at) const;
  esp_err_t tick(uint64_t now, bool render = true);
  esp_err_t blank();
  const animation_definitions &definitions() const { return _definitions; }
private:
  struct layer {
    std::unique_ptr<animation_effect_instance_base> effect;
    bool active = false;
    bool begun = false;
    uint64_t started = 0;
    uint64_t elapsed = 0;
  };
  int find(uint32_t id) const;
  animation_definitions _definitions{};
  std::array<layer, 32> _layers;
  std::array<animation_command, 64> _pending{};
  size_t _pending_count = 0;
  driver_resolver _resolver;
};

class evaluator {
public:
  using command_sink = bool (*)(void *, const animation_command &);
  void configure(const matrix_config *config, uint64_t session, uint64_t revision);
  void tick(uint64_t now, const trigger_gpio_edge_t *edges, size_t edge_count,
            uint64_t edge_time, command_sink sink, void *arg);
private:
  const matrix_config *_config = nullptr;
  uint64_t _session = 0, _revision = 0;
  std::array<uint64_t, 32> _starts{};
  std::array<bool, 32> _active{};
};
} // namespace matrix
