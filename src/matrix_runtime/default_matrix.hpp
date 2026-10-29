#pragma once

#include "matrix_core.hpp"
#include <cstdio>
#include <cstring>
#include <new>

namespace matrix {
inline bool add_default_output(matrix_config &config, const uint8_t mac[6], uint32_t driver, uint32_t pixels,
    uint32_t source_index = 0, lighting_driver_kind kind = lighting_driver_kind::UNKNOWN) {
  if (!pixels || pixels > 65536 || !driver || driver > MAX_DRIVERS_PER_NODE || source_index > 1) return false;
  for (size_t i = 0; i < config.layout_count; ++i) {
    auto &row = config.layout[i];
    if (row.driver != driver || row.source_index != source_index || memcmp(row.node_mac, mac, 6)) continue;
    if (!row.from && row.to == pixels) return false;
    row.from = 0; row.to = pixels; return true;
  }
  if (config.layout_count >= MAX_LAYOUT_SOURCES) return false;
  auto &row = config.layout[config.layout_count++]; row = {};
  memcpy(row.node_mac, mac, 6); row.driver = static_cast<mesh_interop_MatrixDriver>(driver);
  row.from = 0; row.to = pixels;
  row.source_index = source_index;
  if (kind == lighting_driver_kind::RGB_LED_MODULE)
    snprintf(row.name, sizeof(row.name), "RGB LED %u · driver %u", unsigned(source_index + 1), unsigned(driver));
  else snprintf(row.name, sizeof(row.name), "LED strip · driver %u", unsigned(driver));
  return true;
}

inline heap_object<matrix_config> make_default_config(const uint8_t mac[6], driver_resolver resolve) {
  auto config = make_heap_object<matrix_config>();
  if (!config) return nullptr;
  config->schema_version = SCHEMA_VERSION; config->factory_default = true;
  for (uint32_t id = 1; id <= MAX_DRIVERS_PER_NODE; ++id) {
    const auto *driver = resolve(id);
    if (driver) for (uint32_t source = 0; source < driver->get_layout_source_count(); ++source)
      add_default_output(*config, mac, id, driver->get_layout_default_pixel_count(source), source, driver->get_kind());
  }
  if (!config->layout_count) return nullptr;
  config->effects_count = 1;
  auto &effect = config->effects[0]; effect.id = 1; effect.kind = mesh_interop_MatrixEffect_Kind_RAINBOW;
  effect.red = effect.green = effect.blue = effect.alpha = 255; effect.period_ms = 8000;
  snprintf(effect.name, sizeof(effect.name), "Rainbow");
  config->sources_count = 1; config->sources[0].id = 1;
  config->sources[0].kind = mesh_interop_MatrixSource_Kind_DEFAULT_STATE;
  config->conditions_count = 1; config->conditions[0] = {1, mesh_interop_MatrixCondition_Kind_ALWAYS};
  config->entries_count = 1; auto &entry = config->entries[0];
  entry.id = entry.effect_id = entry.source_id = entry.condition_id = 1;
  snprintf(entry.name, sizeof(entry.name), "Default rainbow");
  return config;
}
} // namespace matrix
