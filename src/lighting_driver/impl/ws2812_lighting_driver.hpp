#pragma once

#include "../lighting_driver.hpp"
#include <esp_check.h>
#include <led_strip.h>

struct ws2812_driver_config {
  gpio_num_t data_pin;
  uint16_t led_count;
};

class ws2812_lighting_driver : public rgb_lighting_driver {
public:
  explicit ws2812_lighting_driver(const ws2812_driver_config &cfg)
      : _cfg(cfg), _pixels(new color[cfg.led_count]()), _ready(false),
        _brightness(255), _strip(nullptr) {}

  ~ws2812_lighting_driver() { delete[] _pixels; }

  esp_err_t init() override {
    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = _cfg.data_pin;
    strip_cfg.max_leds = _cfg.led_count;
    strip_cfg.led_model = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;
    rmt_cfg.mem_block_symbols = 64; // default; ESP32 has 8 channels × 64 symbols total

    ESP_RETURN_ON_ERROR(
        led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &_strip), TAG,
        "led_strip_new_rmt_device failed");

    _ready = true;
    return ESP_OK;
  }

  esp_err_t deinit() override {
    _ready = false;
    if (_strip) {
      led_strip_del(_strip);
      _strip = nullptr;
    }
    return ESP_OK;
  }

  size_t get_pixel_count() const override { return _cfg.led_count; }

  esp_err_t set_pixel(uint16_t index, alpha_color c) override {
    if (index >= _cfg.led_count)
      return ESP_ERR_INVALID_ARG;
    _pixels[index] =
        c.alpha == 255 ? c.base_color : c.blend_with(_pixels[index]);
    return ESP_OK;
  }

  esp_err_t fill(alpha_color c) override {
    for (uint16_t i = 0; i < _cfg.led_count; ++i)
      _pixels[i] = c.alpha == 255 ? c.base_color : c.blend_with(_pixels[i]);
    return ESP_OK;
  }

  esp_err_t clear() override { return fill(alpha_color(color(0, 0, 0))); }

  esp_err_t set_global_brightness(uint8_t brightness) override {
    _brightness = brightness;
    return ESP_OK;
  }

  esp_err_t show() override {
    if (!_ready)
      return ESP_ERR_INVALID_STATE;
    for (uint16_t i = 0; i < _cfg.led_count; ++i) {
      uint8_t r = static_cast<uint16_t>(_pixels[i].r) * _brightness / 255u;
      uint8_t g = static_cast<uint16_t>(_pixels[i].g) * _brightness / 255u;
      uint8_t b = static_cast<uint16_t>(_pixels[i].b) * _brightness / 255u;
      ESP_RETURN_ON_ERROR(led_strip_set_pixel(_strip, i, r, g, b), TAG,
                          "led_strip_set_pixel failed");
    }
    return led_strip_refresh(_strip);
  }

  bool _is_ready() const override { return _ready; }

private:
  static constexpr const char *TAG = "ws2812";

  ws2812_driver_config _cfg;
  color *_pixels;
  bool _ready;
  uint8_t _brightness;
  led_strip_handle_t _strip;
};
