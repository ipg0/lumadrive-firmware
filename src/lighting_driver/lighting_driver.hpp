#pragma once

#include "../color/color.hpp"
#include <cstddef>
#include <cstdint>
#include <esp_err.h>

enum class lighting_driver_kind : uint8_t { UNKNOWN = 0, LED_STRIP = 1, RGB_LED_MODULE = 2 };

class rgb_lighting_driver {
public:
  virtual ~rgb_lighting_driver() = default;
  virtual esp_err_t init() = 0;
  virtual esp_err_t deinit() = 0;
  virtual size_t get_pixel_count() const = 0;
  virtual size_t get_canvas_center() const { return get_pixel_count() / 2; }
  virtual lighting_driver_kind get_kind() const { return lighting_driver_kind::UNKNOWN; }
  virtual size_t get_layout_source_count() const { return 1; }
  virtual size_t get_layout_pixel_count(uint32_t source) const { return source ? 0 : get_pixel_count(); }
  virtual size_t get_layout_pixel_offset(uint32_t) const { return 0; }
  virtual size_t get_layout_max_pixel_count(uint32_t source) const { return get_layout_pixel_count(source); }
  virtual size_t get_layout_default_pixel_count(uint32_t source) const { return get_layout_pixel_count(source); }
  virtual esp_err_t reserve_layout_pixels(uint32_t source, size_t count) {
    return count && count <= get_layout_pixel_count(source) ? ESP_OK : ESP_ERR_INVALID_ARG;
  }
  // A canvas view samples only its physical pixels, using global coordinates.
  virtual size_t get_render_pixel_count() const { return get_pixel_count(); }
  virtual uint16_t get_render_pixel_index(uint16_t pixel) const { return pixel; }
  virtual esp_err_t set_pixel(uint16_t, alpha_color) = 0;
  virtual esp_err_t fill(alpha_color) = 0;
  virtual esp_err_t clear() = 0;
  virtual esp_err_t set_global_brightness(uint8_t) = 0;
  virtual esp_err_t show() = 0;
  virtual bool _is_ready() const = 0;
};
