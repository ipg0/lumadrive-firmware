#pragma once

#include "impl/remote_sensor_source.hpp"
#include "impl/virtual_signal_source.hpp"
#include "source_registry.hpp"

namespace signal_source {
enum class source_kind : uint8_t { VIRTUAL, REMOTE_SENSOR };
struct source_config {
  source_kind kind;
  virtual_config generator{};
  uint8_t node_mac[6]{};
  uint32_t sensor_id = 0;
};

// Keep source construction here so consumers sample only through source.
inline source_ptr make_configured_source(const source_config& config, sensor_lookup lookup,
                                         void* context) {
  switch (config.kind) {
  case source_kind::VIRTUAL:
    return make_source<virtual_source>(config.generator);
  case source_kind::REMOTE_SENSOR:
    return make_source<remote_sensor_source>(config.node_mac, config.sensor_id, lookup, context);
  }
  return {};
}
} // namespace signal_source
