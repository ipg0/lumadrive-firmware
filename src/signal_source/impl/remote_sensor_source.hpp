#pragma once

#include "../signal_source.hpp"
#include <cstring>

namespace signal_source {
using sensor_lookup = bool (*)(void*, const uint8_t*, uint32_t, uint64_t, float&);

// Adapts a node's submitted samples to the same interface as any other source.
class remote_sensor_source final : public source {
public:
  remote_sensor_source(const uint8_t* mac, uint32_t id, sensor_lookup lookup, void* context)
      : _id(id), _lookup(lookup), _context(context) {
    std::memcpy(_mac, mac, sizeof(_mac));
  }
  bool sample(uint64_t master_time_us, float& value) override {
    return _lookup && _lookup(_context, _mac, _id, master_time_us, value);
  }

private:
  uint8_t _mac[6]{};
  uint32_t _id;
  sensor_lookup _lookup;
  void* _context;
};
} // namespace signal_source
