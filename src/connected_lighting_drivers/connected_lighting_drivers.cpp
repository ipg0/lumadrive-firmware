#include "connected_lighting_drivers.hpp"
#include "../lighting_driver/impl/bcm_74hc595_lighting_driver.hpp"
#include "../lighting_driver/impl/ws2812_lighting_driver.hpp"
#include <esp_check.h>

namespace {

constexpr const char *TAG = "connected_lighting_drivers";

// Connected hardware: update these pins and bit mappings to match the board.
constexpr ws2812_driver_config WS2812_CFG = {
    .data_pin = GPIO_NUM_19,
    .led_count = 60,
};

constexpr hc595_driver_config HC595_CFG = {
    .ser_pin = GPIO_NUM_23,
    .srclk_pin = GPIO_NUM_18,
    .rclk_pin = GPIO_NUM_5,
    .led_count = 2,
    .leds =
        {
            {.r_bit = 0,
             .g_bit = 1,
             .b_bit = 2,
             .com_bit = 3,
             .common_is_anode = true},
            {.r_bit = 4,
             .g_bit = 5,
             .b_bit = 6,
             .com_bit = 7,
             .common_is_anode = true},
        },
    .bcm_base_period_us = 39,
};

} // namespace

namespace connected_lighting_drivers {

bool uses_gpio(uint32_t pin) {
  return pin == static_cast<uint32_t>(WS2812_CFG.data_pin) ||
         pin == static_cast<uint32_t>(HC595_CFG.ser_pin) ||
         pin == static_cast<uint32_t>(HC595_CFG.srclk_pin) ||
         pin == static_cast<uint32_t>(HC595_CFG.rclk_pin);
}

rgb_lighting_driver *find(uint32_t id) {
  if (id == 1) return &ws2812();
  if (id == 2) return &hc595();
  return nullptr;
}

rgb_lighting_driver &ws2812() {
  static ws2812_lighting_driver driver(WS2812_CFG);
  return driver;
}

rgb_lighting_driver &hc595() {
  static bcm_74hc595_lighting_driver driver(HC595_CFG);
  return driver;
}

esp_err_t init() {
  ESP_RETURN_ON_ERROR(ws2812().init(), TAG, "Failed to initialize WS2812 driver");
  ESP_RETURN_ON_ERROR(hc595().init(), TAG, "Failed to initialize 74HC595 driver");
  return ESP_OK;
}

} // namespace connected_lighting_drivers
