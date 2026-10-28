#pragma once

#include "../color/color.hpp"
#include <cstddef>
#include <cstdint>
#include <esp_err.h>

class rgb_lighting_driver {
public:
  virtual ~rgb_lighting_driver() = default;
  virtual esp_err_t init() = 0;
  virtual esp_err_t deinit() = 0;
  virtual size_t get_pixel_count() const = 0;
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
