#pragma once
#include "connected_lighting_drivers/driver_provisioning.hpp"

// Copy this file for each node. IDs must stay stable across re-provisioning.
constexpr driver_provisioning::configuration make_driver_configuration() {
  using namespace driver_provisioning;
  configuration config{};
  config.count = 1;
  config.drivers[0] = {.id = 1, .type = WS2812, .data_pin = 19, .default_pixels = 60};
  // Example TPIC6B595, with two independent RGB LEDs and common anodes at +12 V:
  // config.count = 2;
  // config.drivers[1] = {.id = 2, .type = HC595, .data_pin = 23,
  //     .clock_pin = 18, .latch_pin = 5, .led_count = 2,
  //     .bcm_base_period_us = 10,
  //     .leds = {{{0, 1, 2, false}, {3, 4, 5, false}}}};
  return config;
}
inline constexpr auto DRIVER_CONFIGURATION = make_driver_configuration();
