#include "connected_lighting_drivers/driver_provisioning.hpp"
#include <array>
#include <cassert>
#include <cstdio>
using namespace driver_provisioning;

configuration fixture() {
  configuration c{}; c.count = 2;
  c.drivers[0] = {.id = 1, .type = WS2812, .data_pin = 19, .default_pixels = 60};
  c.drivers[1] = {.id = 2, .type = HC595, .data_pin = 23, .clock_pin = 18,
      .latch_pin = 5, .led_count = 2, .bcm_base_period_us = 10,
      .leds = {{{0, 1, 2, false}, {3, 4, 5, true}}}};
  return c;
}
int main() {
  auto c = fixture(); std::array<uint8_t, MAX_IMAGE_SIZE> bytes{};
  const auto n = encode(c, bytes.data(), bytes.size()); assert(n == HEADER_SIZE + 2 * RECORD_SIZE);
  configuration result{}; assert(decode(bytes.data(), n, result));
  assert(result.count == 2 && result.drivers[0].default_pixels == 60);
  assert(result.drivers[1].led_count == 2 && result.drivers[1].leds[1].active_low);
  assert(result.drivers[1].leds[1].blue == 5);
  assert(!decode(bytes.data(), n - 1, result));
  bytes[HEADER_SIZE + 2] ^= 1; assert(!decode(bytes.data(), n, result)); bytes[HEADER_SIZE + 2] ^= 1;
  bytes[4] = 2; assert(!decode(bytes.data(), n, result)); bytes[4] = 1;
  c = fixture(); c.drivers[1].id = 1; assert(!validate(c));
  c = fixture(); c.drivers[1].latch_pin = 19; assert(!validate(c));
  c = fixture(); c.drivers[0].data_pin = 16; assert(!validate(c));
  c = fixture(); c.drivers[0].data_pin = 34; assert(!validate(c));
  c = fixture(); c.drivers[0].default_pixels = 2049; assert(!validate(c));
  c = fixture(); c.drivers[1].leds[1].blue = 0; assert(!validate(c));
  c = fixture(); c.count = 3; c.drivers[2] = c.drivers[1]; c.drivers[2].id = 3;
  c.drivers[2].data_pin = 25; c.drivers[2].clock_pin = 26; c.drivers[2].latch_pin = 27;
  c.drivers[2].bcm_base_period_us = 11; assert(!validate(c));
  c = {}; assert(encode(c, bytes.data(), bytes.size()) == HEADER_SIZE);
  assert(decode(bytes.data(), HEADER_SIZE, result) && result.count == 0);
  std::puts("Driver provisioning: round trips, corruption, pin and resource validation passed.");
}
