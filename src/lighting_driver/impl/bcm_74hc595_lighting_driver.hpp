#pragma once

#include "../lighting_driver.hpp"
#include <cstring>
#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>

struct hc595_led_config {
  uint8_t r_bit;
  uint8_t g_bit;
  uint8_t b_bit;
  uint8_t com_bit;
  bool common_is_anode; // true: COM→VCC (active-low channels), false: COM→GND
                        // (active-high channels)
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
      : _cfg(cfg), _ready(false), _brightness(255), _bcm_plane(0),
        _timer(nullptr) {
    memset(_pixels, 0, sizeof(_pixels));
  }

  esp_err_t init() override {
    gpio_config_t io_conf = {};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << _cfg.ser_pin) | (1ULL << _cfg.srclk_pin) |
                           (1ULL << _cfg.rclk_pin);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "gpio_config failed");

    gpio_set_level(_cfg.ser_pin, 0);
    gpio_set_level(_cfg.srclk_pin, 0);
    gpio_set_level(_cfg.rclk_pin, 0);
    shift_out(0);

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = bcm_timer_cb;
    timer_args.arg = this;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "bcm_hc595";
    timer_args.skip_unhandled_events = true;
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &_timer), TAG,
                        "esp_timer_create failed");

    _ready = true;
    return esp_timer_start_once(_timer, _cfg.bcm_base_period_us);
  }

  esp_err_t deinit() override {
    _ready = false;
    if (_timer) {
      esp_timer_stop(_timer);
      esp_timer_delete(_timer);
      _timer = nullptr;
    }
    shift_out(all_off_byte());
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
    for (uint8_t i = 0; i < _cfg.led_count; ++i)
      _pixels[i] = c.alpha == 255 ? c.base_color : c.blend_with(_pixels[i]);
    return ESP_OK;
  }

  esp_err_t clear() override { return fill(alpha_color(color(0, 0, 0))); }

  esp_err_t set_global_brightness(uint8_t brightness) override {
    _brightness = brightness;
    return ESP_OK;
  }

  // BCM runs continuously from the timer; pixels are picked up on the next
  // bit-plane.
  esp_err_t show() override { return ESP_OK; }

  bool _is_ready() const override { return _ready; }

private:
  static constexpr const char *TAG = "bcm_74hc595";

  hc595_driver_config _cfg;
  color _pixels[2];
  bool _ready;
  uint8_t _brightness;
  uint8_t _bcm_plane;
  esp_timer_handle_t _timer;

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

      if (led.common_is_anode) {
        // COM is held high and the LED channel is active-low:
        // bit=1 => channel low (LED on), bit=0 => channel high (LED off).
        byte |= (1u << led.com_bit);
        if (((r >> plane) & 1u) == 0u) byte |= (1u << led.r_bit);
        if (((g >> plane) & 1u) == 0u) byte |= (1u << led.g_bit);
        if (((b >> plane) & 1u) == 0u) byte |= (1u << led.b_bit);
      } else {
        // COM is held low and the LED channel is active-high:
        // bit=1 => channel high (LED on), bit=0 => channel low (LED off).
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
      if (led.common_is_anode) {
        // Common-anode LEDs are off with COM high and each channel high.
        byte |= (1u << led.r_bit) | (1u << led.g_bit) | (1u << led.b_bit) |
                (1u << led.com_bit);
      } else {
        // Common-cathode LEDs are off with COM low and each channel low.
        // Leave the byte at 0 for this case.
      }
    }
    return byte;
  }

  void shift_out(uint8_t data) const {
    for (int8_t bit = 7; bit >= 0; --bit) {
      gpio_set_level(_cfg.ser_pin, (data >> bit) & 1u);
      gpio_set_level(_cfg.srclk_pin, 1);
      gpio_set_level(_cfg.srclk_pin, 0);
    }
    gpio_set_level(_cfg.rclk_pin, 1);
    gpio_set_level(_cfg.rclk_pin, 0);
  }

  static void bcm_timer_cb(void *arg) {
    auto *self = static_cast<bcm_74hc595_lighting_driver *>(arg);
    uint8_t plane = self->_bcm_plane;
    uint8_t byte = self->compute_byte(plane);
    self->shift_out(byte);
    // hold this plane for 2^plane units, then move to the next
    esp_timer_start_once(self->_timer,
                         (1u << plane) * self->_cfg.bcm_base_period_us);
    self->_bcm_plane = (plane + 1u) % 8u;
  }
};