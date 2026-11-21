#include "hardware_config/hardware_config.hpp"
#include <array>
#include <cassert>
#include <cstdio>
using namespace hardware_config;

configuration fixture() {
  configuration c{}; c.count = 2; c.role = NODE_MASTER;
  c.drivers[0] = {.id = 1, .type = WS2812, .data_pin = 19, .default_pixels = 60};
  c.drivers[1] = {.id = 2, .type = HC595, .data_pin = 23, .clock_pin = 18,
      .latch_pin = 5, .led_count = 2, .bcm_base_period_us = 10,
      .leds = {{{0, 1, 2, false}, {3, 4, 5, true}}}};
  return c;
}
int main() {
  auto c = fixture(); std::array<uint8_t, MAX_IMAGE_SIZE> bytes{};
  const auto n = encode(c, bytes.data(), bytes.size()); assert(n == HEADER_SIZE + 2 * RECORD_SIZE + 1);
  configuration result{}; assert(decode(bytes.data(), n, result));
  assert(result.count == 2 && result.drivers[0].default_pixels == 60);
  assert(result.role == NODE_MASTER);
  assert(result.drivers[1].led_count == 2 && result.drivers[1].leds[1].active_low);
  assert(result.drivers[1].leds[1].blue == 5);
  assert(!decode(bytes.data(), n - 1, result));
  bytes[HEADER_SIZE + 2] ^= 1; assert(!decode(bytes.data(), n, result)); bytes[HEADER_SIZE + 2] ^= 1;
  bytes[4] = 3; assert(!decode(bytes.data(), n, result));
  c = fixture(); c.gpio_count = 3;
  c.gpios[0] = {12, PULL_UP, true}; c.gpios[1] = {13, PULL_DOWN}; c.gpios[2] = {34, PULL_NONE};
  const auto inputs_size = encode(c, bytes.data(), bytes.size());
  assert(inputs_size == HEADER_SIZE + 2 * RECORD_SIZE + 6 + 1);
  assert(decode(bytes.data(), inputs_size, result));
  assert(result.gpio_count == 3 && result.gpios[0].pull == PULL_UP && result.gpios[1].pull == PULL_DOWN && result.gpios[2].pin == 34);
  assert(result.gpios[0].inverted && !result.gpios[1].inverted);
  bytes[inputs_size - 1] = NODE_UNCONFIGURED; put(bytes.data() + 12, image_checksum(bytes.data(), inputs_size), 4);
  assert(!decode(bytes.data(), inputs_size, result));
  bytes[inputs_size - 1] = NODE_SLAVE;
  put(bytes.data() + 12, image_checksum(bytes.data(), inputs_size), 4);
  assert(decode(bytes.data(), inputs_size, result) && result.role == NODE_SLAVE && result.gpios[0].inverted);
  c.gpios[1].pin = 12; assert(!validate(c));
  c.gpios[1].pin = 19; assert(!validate(c));
  c.gpios[1].pin = 4; assert(validate(c));
  c.gpios[1].pin = 16; assert(!validate(c));
  c.gpios[1].pin = 6; assert(!validate(c));
  c.gpios[1].pin = 35; assert(!validate(c)); // Input-only pins have no internal pulls.
  c.gpios[1] = {13, 3}; assert(!validate(c));
  c = fixture(); c.drivers[1].id = 1; assert(!validate(c));
  c = fixture(); c.drivers[1].latch_pin = 19; assert(!validate(c));
  c = fixture(); c.drivers[0].data_pin = 16; assert(!validate(c));
  c = fixture(); c.drivers[0].data_pin = 34; assert(!validate(c));
  c = fixture(); c.drivers[0].default_pixels = 2049; assert(!validate(c));
  c = fixture(); c.drivers[1].leds[1].blue = 0; assert(!validate(c));
  c = fixture(); c.count = 3; c.drivers[2] = c.drivers[1]; c.drivers[2].id = 3;
  c.drivers[2].data_pin = 25; c.drivers[2].clock_pin = 26; c.drivers[2].latch_pin = 27;
  c.drivers[2].bcm_base_period_us = 11; assert(!validate(c));
  c = {}; assert(!encode(c, bytes.data(), bytes.size()));
  c.role = NODE_SLAVE; assert(encode(c, bytes.data(), bytes.size()) == HEADER_SIZE + 1);
  assert(decode(bytes.data(), HEADER_SIZE + 1, result) && result.count == 0 && result.role == NODE_SLAVE);
  c.role = 3; assert(!validate(c));
  std::puts("Hardware config: roles, corruption, pin and resource validation passed.");
}
