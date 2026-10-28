#include "connected_lighting_drivers.hpp"
#include "../lighting_driver/impl/bcm_74hc595_lighting_driver.hpp"
#include "../lighting_driver/impl/ws2812_lighting_driver.hpp"
#include <esp_check.h>

namespace {

constexpr const char *TAG = "connected_lighting_drivers";

// Connected hardware: update these pins and bit mappings to match the board.
constexpr ws2812_driver_config ws2812_cfg = {
    .data_pin = GPIO_NUM_19,
    .led_count = 60,
};

constexpr hc595_driver_config hc595_cfg = {
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

rgb_lighting_driver &ws2812() {
  static ws2812_lighting_driver driver(ws2812_cfg);
  return driver;
}

rgb_lighting_driver &hc595() {
  static bcm_74hc595_lighting_driver driver(hc595_cfg);
  return driver;
}

esp_err_t init() {
  ESP_RETURN_ON_ERROR(ws2812().init(), TAG, "Failed to initialize WS2812 driver");
  ESP_RETURN_ON_ERROR(hc595().init(), TAG, "Failed to initialize 74HC595 driver");
  return ESP_OK;
}

} // namespace connected_lighting_drivers
