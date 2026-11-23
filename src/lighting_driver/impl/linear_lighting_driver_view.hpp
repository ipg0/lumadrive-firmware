#pragma once

#include "../lighting_driver.hpp"
#include <algorithm>

// Borrow a physical driver as a window into a shared linear canvas. Endpoints
// are boundaries, so reversal preserves exactly the same covered pixels.
class linear_lighting_driver_view : public rgb_lighting_driver {
public:
  linear_lighting_driver_view(rgb_lighting_driver& driver, uint32_t from, uint32_t to,
                              uint32_t canvas_size, uint32_t source_index = 0,
                              uint32_t center_index = UINT32_MAX, uint32_t region_start = 0,
                              uint32_t region_end = UINT32_MAX)
      : _driver(driver), _from(from), _to(to), _canvas_size(canvas_size),
        _source_index(source_index),
        _center_index(center_index == UINT32_MAX ? canvas_size / 2 : center_index),
        _region_start(region_start),
        _region_end(region_end == UINT32_MAX ? canvas_size : region_end) {}

  bool valid() const {
    return _canvas_size && _canvas_size <= 65536 && _from != _to &&
           std::max(_from, _to) <= _canvas_size && _region_start < _region_end &&
           _region_end <= _canvas_size && _source_index < _driver.get_layout_source_count() &&
           std::max(_from, _to) - std::min(_from, _to) <=
               _driver.get_layout_pixel_count(_source_index);
  }
  esp_err_t init() override { return valid() ? ESP_OK : ESP_ERR_INVALID_ARG; }
  esp_err_t deinit() override { return ESP_OK; }
  size_t get_pixel_count() const override { return _region_end - _region_start; }
  size_t get_canvas_center() const override {
    return std::clamp(_center_index, _region_start, _region_end) - _region_start;
  }
  size_t get_render_pixel_count() const override {
    if (!valid())
      return 0;
    const auto first = std::max(std::min(_from, _to), _region_start);
    const auto last = std::min(std::max(_from, _to), _region_end);
    return last > first ? last - first : 0;
  }
  uint16_t get_render_pixel_index(uint16_t pixel) const override {
    const auto first = std::max(std::min(_from, _to), _region_start);
    const auto last = std::min(std::max(_from, _to), _region_end);
    return static_cast<uint16_t>((_from < _to ? first + pixel : last - 1 - pixel) - _region_start);
  }
  esp_err_t set_pixel(uint16_t pixel, alpha_color value) override {
    if (!valid() || pixel >= get_pixel_count())
      return ESP_ERR_INVALID_ARG;
    const uint32_t global = uint32_t(pixel) + _region_start;
    if (global < std::min(_from, _to) || global >= std::max(_from, _to))
      return ESP_OK;
    const auto local = _from < _to ? global - _from : _from - 1 - global;
    return _driver.set_pixel(
        static_cast<uint16_t>(_driver.get_layout_pixel_offset(_source_index) + local), value);
  }
  esp_err_t fill(alpha_color value) override {
    if (!valid())
      return ESP_ERR_INVALID_ARG;
    const auto count = get_render_pixel_count();
    if (count == _driver.get_pixel_count() && count == std::max(_from, _to) - std::min(_from, _to))
      return _driver.fill(value);
    for (size_t i = 0; i < count; ++i) {
      auto error = set_pixel(get_render_pixel_index(static_cast<uint16_t>(i)), value);
      if (error != ESP_OK)
        return error;
    }
    return ESP_OK;
  }
  esp_err_t clear() override { return fill(alpha_color(color(0, 0, 0))); }
  esp_err_t show() override { return _driver.show(); }
  esp_err_t set_global_brightness(uint8_t brightness) override {
    return _driver.set_global_brightness(brightness);
  }
  bool _is_ready() const override { return valid() && _driver._is_ready(); }

private:
  rgb_lighting_driver& _driver;
  uint32_t _from, _to, _canvas_size, _source_index, _center_index, _region_start, _region_end;
};
