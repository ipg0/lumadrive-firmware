#pragma once

#include "../lighting_driver.hpp"
#include <initializer_list>
#include <driver/gpio.h>
#include <driver/gptimer.h>
#include <esp_attr.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

struct hc595_led_config {
  uint8_t r_bit;
  uint8_t g_bit;
  uint8_t b_bit;
  // The LED common is wired externally, never to a shift-register output.
  // TPIC6B595 sinks current for a stored 1, so it uses active-high data even
  // though its LEDs have common anodes connected to the positive supply.
  bool channels_active_low = false;
};

struct hc595_driver_config {
  gpio_num_t ser_pin;
  gpio_num_t srclk_pin;
  gpio_num_t rclk_pin;
  uint8_t led_count; // 1 or 2
  hc595_led_config leds[2];
  uint32_t bcm_base_period_us;
};

class bcm_74hc595_lighting_driver : public rgb_lighting_driver {
public:
  explicit bcm_74hc595_lighting_driver(const hc595_driver_config &cfg)
      : _cfg(cfg), _pixels{}, _ready(false), _brightness(255) {}

  esp_err_t init() override;
  esp_err_t deinit() override;

  size_t get_pixel_count() const override { return _cfg.led_count; }
  lighting_driver_kind get_kind() const override { return lighting_driver_kind::RGB_LED_MODULE; }
  size_t get_layout_source_count() const override { return _cfg.led_count; }
  size_t get_layout_pixel_count(uint32_t source) const override { return source < _cfg.led_count ? 1 : 0; }
  size_t get_layout_pixel_offset(uint32_t source) const override { return source; }
  bool uses_gpio(uint32_t pin) const {
    return pin == static_cast<uint32_t>(_cfg.ser_pin) || pin == static_cast<uint32_t>(_cfg.srclk_pin) ||
        pin == static_cast<uint32_t>(_cfg.rclk_pin);
  }

  esp_err_t set_pixel(uint16_t index, alpha_color c) override {
    if (index >= _cfg.led_count)
      return ESP_ERR_INVALID_ARG;
    _pixels[index] =
        c.alpha == 255 ? c.base_color : c.blend_with(_pixels[index]);
    return ESP_OK;
  }

  esp_err_t fill(alpha_color c) override {
    for (uint8_t i = 0; i < _cfg.led_count; ++i)
      _pixels[i] = c.alpha == 255 ? c.base_color : c.blend_with(_pixels[i]);
    return ESP_OK;
  }

  esp_err_t clear() override { return fill(alpha_color(color(0, 0, 0))); }

  esp_err_t set_global_brightness(uint8_t brightness) override {
    _brightness = brightness;
    return ESP_OK;
  }

  // Rendering edits only the working pixels. Publish complete bit planes,
  // then let the ISR adopt them together at the next BCM cycle boundary.
  esp_err_t show() override {
    if (!_ready) return ESP_ERR_INVALID_STATE;
    uint8_t planes[8];
    for (uint8_t plane = 0; plane < 8; ++plane) planes[plane] = compute_byte(plane);
    portENTER_CRITICAL(&_frame_lock);
    for (unsigned plane = 0; plane < 8; ++plane) _pending_planes[plane] = planes[plane];
    _pending_frame = true;
    portEXIT_CRITICAL(&_frame_lock);
    return ESP_OK;
  }

  bool _is_ready() const override { return _ready; }

private:
  static constexpr const char *TAG = "bcm_74hc595";

  hc595_driver_config _cfg;
  color _pixels[2];
  bool _ready;
  uint8_t _brightness;
  struct scheduler;
  static scheduler _scheduler;
  uint8_t _active_planes[8]{}, _pending_planes[8]{};
  bool _pending_frame = false;
  portMUX_TYPE _frame_lock = portMUX_INITIALIZER_UNLOCKED;

  bool valid_config() const {
    if (!_cfg.led_count || _cfg.led_count > 2 || !_cfg.bcm_base_period_us) return false;
    uint8_t used = 0;
    for (uint8_t i = 0; i < _cfg.led_count; ++i) {
      const auto &led = _cfg.leds[i];
      for (const auto bit : {led.r_bit, led.g_bit, led.b_bit}) {
        if (bit >= 8 || (used & (1u << bit))) return false;
        used |= (1u << bit);
      }
    }
    return true;
  }

  uint8_t apply_brightness(uint8_t ch) const {
    return static_cast<uint16_t>(ch) * _brightness / 255u;
  }

  uint8_t compute_byte(uint8_t plane) const {
    uint8_t byte = 0;
    for (uint8_t i = 0; i < _cfg.led_count; ++i) {
      const hc595_led_config &led = _cfg.leds[i];
      const uint8_t r = apply_brightness(_pixels[i].r);
      const uint8_t g = apply_brightness(_pixels[i].g);
      const uint8_t b = apply_brightness(_pixels[i].b);

      if (led.channels_active_low) {
        // Stored 0 turns the channel on (e.g. a directly driven common-anode
        // LED on an ordinary 74HC595 with its common connected externally).
        if (((r >> plane) & 1u) == 0u) byte |= (1u << led.r_bit);
        if (((g >> plane) & 1u) == 0u) byte |= (1u << led.g_bit);
        if (((b >> plane) & 1u) == 0u) byte |= (1u << led.b_bit);
      } else {
        // Stored 1 turns the channel on, including TPIC6B595 low-side sinks.
        if (((r >> plane) & 1u) != 0u) byte |= (1u << led.r_bit);
        if (((g >> plane) & 1u) != 0u) byte |= (1u << led.g_bit);
        if (((b >> plane) & 1u) != 0u) byte |= (1u << led.b_bit);
      }
    }
    return byte;
  }

  uint8_t all_off_byte() const {
    uint8_t byte = 0;
    for (uint8_t i = 0; i < _cfg.led_count; ++i) {
      const hc595_led_config &led = _cfg.leds[i];
      if (led.channels_active_low) {
        byte |= (1u << led.r_bit) | (1u << led.g_bit) | (1u << led.b_bit);
      }
    }
    return byte;
  }

  void IRAM_ATTR shift_out(uint8_t data) const;
  static bool IRAM_ATTR bcm_timer_cb(gptimer_handle_t timer,
                                    const gptimer_alarm_event_data_t *, void *arg);
};
