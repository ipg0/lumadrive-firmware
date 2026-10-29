#pragma once

#include "../lighting_driver.hpp"
#include <driver/gpio.h>
#include <driver/rmt_tx.h>
#include <driver/rmt_encoder.h>
#include <esp_attr.h>

struct ws2812_driver_config {
  gpio_num_t data_pin;
  uint16_t led_count;
  size_t rmt_mem_block_symbols = 64;
};

class ws2812_lighting_driver : public rgb_lighting_driver {
public:
  static constexpr size_t MAX_PIXELS = 2048;
  explicit ws2812_lighting_driver(const ws2812_driver_config &cfg) : _cfg(cfg) {}
  ~ws2812_lighting_driver();
  esp_err_t init() override;
  esp_err_t deinit() override;
  size_t get_pixel_count() const override { return _capacity; }
  lighting_driver_kind get_kind() const override { return lighting_driver_kind::LED_STRIP; }
  size_t get_layout_max_pixel_count(uint32_t source) const override { return source ? 0 : MAX_PIXELS; }
  size_t get_layout_default_pixel_count(uint32_t source) const override { return source ? 0 : _cfg.led_count; }
  esp_err_t reserve_layout_pixels(uint32_t source, size_t count) override;
  bool uses_gpio(uint32_t pin) const { return pin == static_cast<uint32_t>(_cfg.data_pin); }
  void set_rmt_memory_symbols(size_t symbols) { if (!_ready) _cfg.rmt_mem_block_symbols = symbols; }
  esp_err_t set_pixel(uint16_t index, alpha_color value) override;
  esp_err_t fill(alpha_color value) override;
  esp_err_t clear() override { return fill(alpha_color(color(0, 0, 0))); }
  esp_err_t set_global_brightness(uint8_t brightness) override { _brightness = brightness; return ESP_OK; }
  esp_err_t show() override;
  bool _is_ready() const override { return _ready; }
private:
  static size_t IRAM_ATTR encode_pixels(const void *data, size_t size, size_t written,
      size_t available, rmt_symbol_word_t *symbols, bool *done, void *);
  ws2812_driver_config _cfg;
  color *_pixels = nullptr;
  uint8_t *_wire_pixels = nullptr;
  size_t _capacity = 0;
  bool _ready = false;
  uint8_t _brightness = 255;
  rmt_channel_handle_t _channel = nullptr;
  rmt_encoder_handle_t _encoder = nullptr;
};
