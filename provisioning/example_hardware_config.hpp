#pragma once
#include "hardware_config/hardware_config.hpp"

// Copy this file for each node. IDs must stay stable across re-provisioning.
constexpr hardware_config::configuration make_hardware_configuration() {
  using namespace hardware_config;
  configuration config{};
  config.role = NODE_MASTER; // Use NODE_SLAVE on slave nodes.
  config.count = 1;
  config.drivers[0] = {.id = 1, .type = WS2812, .data_pin = 19, .default_pixels = 60};
  // Example TPIC6B595, with two independent RGB LEDs and common anodes at +12 V:
  // config.count = 2;
  // config.drivers[1] = {.id = 2, .type = HC595, .data_pin = 23,
  //     .clock_pin = 18, .latch_pin = 5, .led_count = 2,
  //     .bcm_base_period_us = 10,
  //     .leds = {{{0, 1, 2, false}, {3, 4, 5, false}}}};
  config.gpio_count = 1;
  config.gpios[0] = {.pin = 12, .pull = PULL_UP, .inverted = true };
  // Optional trigger inputs; GPIO34–39 have no internal pull resistors.
  // config.gpio_count = 2;
  // config.gpios[0] = {.pin = 12, .pull = PULL_UP, .inverted = true};
  // config.gpios[1] = {.pin = 34, .pull = PULL_NONE};
  return config;
}
inline constexpr auto HARDWARE_CONFIGURATION = make_hardware_configuration();
