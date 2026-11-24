#pragma once

#include "../color/color.hpp"
#include <cstddef>
#include <cstdint>
#include <esp_err.h>

// A canvas that effects can paint without owning or controlling hardware.
class render_target {
public:
  virtual ~render_target() = default;
  virtual size_t get_pixel_count() const = 0;
  virtual size_t get_canvas_center() const { return get_pixel_count() / 2; }
  virtual size_t get_render_pixel_count() const { return get_pixel_count(); }
  virtual uint16_t get_render_pixel_index(uint16_t pixel) const { return pixel; }
  virtual esp_err_t set_pixel(uint16_t, alpha_color) = 0;
  virtual esp_err_t fill(alpha_color) = 0;
};
