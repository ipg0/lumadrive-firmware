#pragma once

#include "../lighting_driver.hpp"
#include <algorithm>

// Borrow a physical driver as a window into a shared linear canvas. Endpoints
// are boundaries, so reversal preserves exactly the same covered pixels.
class linear_lighting_driver_view : public rgb_lighting_driver {
public:
  linear_lighting_driver_view(rgb_lighting_driver &driver, uint32_t from,
                              uint32_t to, uint32_t canvas_size, uint32_t source_index = 0,
                              uint32_t center_index = UINT32_MAX)
      : _driver(driver), _from(from), _to(to), _canvas_size(canvas_size), _source_index(source_index),
        _center_index(center_index == UINT32_MAX ? canvas_size / 2 : center_index) {}

  bool valid() const {
    return _canvas_size && _canvas_size <= 65536 && _from != _to &&
        std::max(_from, _to) <= _canvas_size &&
        _source_index < _driver.get_layout_source_count() &&
        std::max(_from, _to) - std::min(_from, _to) <= _driver.get_layout_pixel_count(_source_index);
  }
  esp_err_t init() override { return valid() ? ESP_OK : ESP_ERR_INVALID_ARG; }
  esp_err_t deinit() override { return ESP_OK; }
  size_t get_pixel_count() const override { return _canvas_size; }
  size_t get_canvas_center() const override { return _center_index; }
  size_t get_render_pixel_count() const override { return valid() ? std::max(_from, _to) - std::min(_from, _to) : 0; }
  uint16_t get_render_pixel_index(uint16_t pixel) const override {
    return static_cast<uint16_t>(_from < _to ? _from + pixel : _from - 1 - pixel);
  }
  esp_err_t set_pixel(uint16_t pixel, alpha_color value) override {
    if (!valid() || pixel >= _canvas_size) return ESP_ERR_INVALID_ARG;
    if (pixel < std::min(_from, _to) || pixel >= std::max(_from, _to)) return ESP_OK;
    const auto local = _from < _to ? pixel - _from : _from - 1 - pixel;
    return _driver.set_pixel(static_cast<uint16_t>(_driver.get_layout_pixel_offset(_source_index) + local), value);
  }
  esp_err_t fill(alpha_color value) override {
    if (!valid()) return ESP_ERR_INVALID_ARG;
    const auto count = get_render_pixel_count();
    if (count == _driver.get_pixel_count()) return _driver.fill(value);
    for (size_t i = 0; i < count; ++i) {
      auto error = _driver.set_pixel(static_cast<uint16_t>(_driver.get_layout_pixel_offset(_source_index) + i), value);
      if (error != ESP_OK) return error;
    }
    return ESP_OK;
  }
  esp_err_t clear() override { return fill(alpha_color(color(0, 0, 0))); }
  esp_err_t show() override { return _driver.show(); }
  esp_err_t set_global_brightness(uint8_t brightness) override { return _driver.set_global_brightness(brightness); }
  bool _is_ready() const override { return valid() && _driver._is_ready(); }
private:
  rgb_lighting_driver &_driver;
  uint32_t _from, _to, _canvas_size, _source_index, _center_index;
};
