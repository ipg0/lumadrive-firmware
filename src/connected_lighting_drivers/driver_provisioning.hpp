#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Stable, explicitly encoded provisioning format. No C++ structure is written
// to flash directly, so compiler padding and firmware updates cannot change it.
namespace driver_provisioning {
constexpr size_t MAX_DRIVERS = 6;
constexpr size_t HEADER_SIZE = 16;
constexpr size_t RECORD_SIZE = 20;
constexpr size_t MAX_IMAGE_SIZE = HEADER_SIZE + MAX_DRIVERS * RECORD_SIZE;
constexpr uint8_t WS2812 = 1, HC595 = 2;

struct led_config { uint8_t red = 0, green = 0, blue = 0; bool active_low = false; };
struct driver_config {
  uint8_t id = 0, type = 0, data_pin = 0, clock_pin = 0, latch_pin = 0;
  uint8_t led_count = 0;
  uint16_t default_pixels = 0;
  uint32_t bcm_base_period_us = 0;
  std::array<led_config, 2> leds{};
};
struct configuration { size_t count = 0; std::array<driver_config, MAX_DRIVERS> drivers{}; };

inline uint32_t get(const uint8_t *p, size_t n) {
  uint32_t result = 0;
  for (size_t i = 0; i < n; ++i) result |= uint32_t(p[i]) << (8 * i);
  return result;
}
inline void put(uint8_t *p, uint32_t value, size_t n) {
  for (size_t i = 0; i < n; ++i) p[i] = uint8_t(value >> (8 * i));
}
inline uint32_t checksum(const uint8_t *data, size_t size) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}
inline bool output_pin(uint8_t pin) {
  switch (pin) {
  case 0: case 2: case 5: case 12: case 13: case 14: case 15:
  case 17: case 18: case 19: case 21: case 22: case 23:
  case 25: case 26: case 27: case 32: case 33: return true;
  default: return false;
  }
}
inline bool validate(const configuration &config) {
  if (config.count > MAX_DRIVERS) return false;
  uint64_t pins = 0; uint8_t ids = 0; uint32_t period = 0;
  for (size_t i = 0; i < config.count; ++i) {
    const auto &d = config.drivers[i];
    if (!d.id || d.id > MAX_DRIVERS || (ids & (1u << d.id))) return false;
    ids |= uint8_t(1u << d.id);
    const uint8_t values[] = {d.data_pin, d.clock_pin, d.latch_pin};
    const size_t count = d.type == WS2812 ? 1 : d.type == HC595 ? 3 : 0;
    if (!count) return false;
    for (size_t j = 0; j < count; ++j) {
      if (!output_pin(values[j]) || (pins & (uint64_t(1) << values[j]))) return false;
      pins |= uint64_t(1) << values[j];
    }
    if (d.type == WS2812) {
      if (!d.default_pixels || d.default_pixels > 2048 || d.led_count || d.clock_pin || d.latch_pin || d.bcm_base_period_us) return false;
    } else {
      if (d.default_pixels || !d.led_count || d.led_count > 2 || !d.bcm_base_period_us ||
          (period && period != d.bcm_base_period_us)) return false;
      period = d.bcm_base_period_us;
      uint8_t bits = 0;
      for (size_t j = 0; j < d.led_count; ++j) {
        for (auto bit : {d.leds[j].red, d.leds[j].green, d.leds[j].blue}) {
          if (bit > 7 || (bits & (1u << bit))) return false;
          bits |= uint8_t(1u << bit);
        }
      }
    }
  }
  return true;
}
inline size_t encode(const configuration &config, uint8_t *out, size_t capacity) {
  if (!validate(config)) return 0;
  const size_t size = HEADER_SIZE + config.count * RECORD_SIZE;
  if (capacity < size) return 0;
  for (size_t i = 0; i < size; ++i) out[i] = 0;
  out[0] = 'L'; out[1] = 'D'; out[2] = 'R'; out[3] = 'V';
  put(out + 4, 1, 2); put(out + 6, config.count, 2); put(out + 8, size, 4);
  for (size_t i = 0; i < config.count; ++i) {
    const auto &d = config.drivers[i]; auto *p = out + HEADER_SIZE + i * RECORD_SIZE;
    p[0] = d.id; p[1] = d.type; p[2] = d.data_pin; p[3] = d.clock_pin; p[4] = d.latch_pin; p[5] = d.led_count;
    put(p + 6, d.default_pixels, 2); put(p + 8, d.bcm_base_period_us, 4);
    for (size_t j = 0; j < 2; ++j) {
      p[12 + j * 3] = d.leds[j].red; p[13 + j * 3] = d.leds[j].green; p[14 + j * 3] = d.leds[j].blue;
      if (d.leds[j].active_low) p[18] |= uint8_t(1u << j);
    }
  }
  put(out + 12, checksum(out + HEADER_SIZE, size - HEADER_SIZE), 4);
  return size;
}
inline bool decode(const uint8_t *data, size_t size, configuration &result) {
  if (size < HEADER_SIZE || data[0] != 'L' || data[1] != 'D' || data[2] != 'R' || data[3] != 'V' ||
      get(data + 4, 2) != 1 || get(data + 6, 2) > MAX_DRIVERS || get(data + 8, 4) != size ||
      size != HEADER_SIZE + get(data + 6, 2) * RECORD_SIZE ||
      get(data + 12, 4) != checksum(data + HEADER_SIZE, size - HEADER_SIZE)) return false;
  configuration parsed{}; parsed.count = get(data + 6, 2);
  for (size_t i = 0; i < parsed.count; ++i) {
    auto &d = parsed.drivers[i]; const auto *p = data + HEADER_SIZE + i * RECORD_SIZE;
    if ((p[18] & ~3u) || p[19]) return false;
    d.id = p[0]; d.type = p[1]; d.data_pin = p[2]; d.clock_pin = p[3]; d.latch_pin = p[4]; d.led_count = p[5];
    d.default_pixels = get(p + 6, 2); d.bcm_base_period_us = get(p + 8, 4);
    for (size_t j = 0; j < 2; ++j) d.leds[j] = {p[12 + j * 3], p[13 + j * 3], p[14 + j * 3], bool(p[18] & (1u << j))};
  }
  if (!validate(parsed)) return false;
  result = parsed; return true;
}
} // namespace driver_provisioning
