#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Stable, explicitly encoded provisioning format. No C++ structure is written
// to flash directly, so compiler padding and firmware updates cannot change it.
namespace hardware_config {
constexpr size_t MAX_DRIVERS = 6;
constexpr size_t MAX_GPIO_INPUTS = 32;
constexpr size_t HEADER_SIZE = 16;
constexpr size_t RECORD_SIZE = 20;
constexpr size_t MAX_IMAGE_SIZE = HEADER_SIZE + MAX_DRIVERS * RECORD_SIZE + MAX_GPIO_INPUTS * 2 + 1;
constexpr uint8_t WS2812 = 1, HC595 = 2;
constexpr uint8_t PULL_NONE = 0, PULL_UP = 1, PULL_DOWN = 2;
constexpr uint8_t NODE_UNCONFIGURED = 0, NODE_MASTER = 1, NODE_SLAVE = 2;

struct led_config { uint8_t red = 0, green = 0, blue = 0; bool active_low = false; };
struct driver_config {
  uint8_t id = 0, type = 0, data_pin = 0, clock_pin = 0, latch_pin = 0;
  uint8_t led_count = 0;
  uint16_t default_pixels = 0;
  uint32_t bcm_base_period_us = 0;
  std::array<led_config, 2> leds{};
};
struct gpio_input_config { uint8_t pin = 0, pull = PULL_NONE; bool inverted = false; };
struct configuration {
  size_t count = 0; std::array<driver_config, MAX_DRIVERS> drivers{};
  size_t gpio_count = 0; std::array<gpio_input_config, MAX_GPIO_INPUTS> gpios{};
  uint8_t role = NODE_UNCONFIGURED;
};

inline uint32_t get(const uint8_t *p, size_t n) {
  uint32_t result = 0;
  for (size_t i = 0; i < n; ++i) result |= uint32_t(p[i]) << (8 * i);
  return result;
}
inline void put(uint8_t *p, uint32_t value, size_t n) {
  for (size_t i = 0; i < n; ++i) p[i] = uint8_t(value >> (8 * i));
}
inline uint32_t image_checksum(const uint8_t *data, size_t size) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < size; ++i) {
    crc ^= (i >= 12 && i < 16) ? 0 : data[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}
inline bool output_pin(uint8_t pin) {
  switch (pin) {
  case 0: case 2: case 4: case 5: case 12: case 13: case 14: case 15:
  case 17: case 18: case 19: case 21: case 22: case 23:
  case 25: case 26: case 27: case 32: case 33: return true;
  default: return false;
  }
}
inline bool input_pin(uint8_t pin) {
  return output_pin(pin) || (pin >= 34 && pin <= 39);
}
inline bool validate(const configuration &config) {
  if (config.count > MAX_DRIVERS || config.gpio_count > MAX_GPIO_INPUTS ||
      (config.role != NODE_MASTER && config.role != NODE_SLAVE)) return false;
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
  for (size_t i = 0; i < config.gpio_count; ++i) {
    const auto &input = config.gpios[i];
    if (!input_pin(input.pin) || input.pull > PULL_DOWN ||
        (input.pin >= 34 && input.pull != PULL_NONE) || (pins & (uint64_t(1) << input.pin))) return false;
    pins |= uint64_t(1) << input.pin;
  }
  return true;
}
inline size_t encode(const configuration &config, uint8_t *out, size_t capacity) {
  if (!validate(config)) return 0;
  const size_t size = HEADER_SIZE + config.count * RECORD_SIZE + config.gpio_count * 2 + 1;
  if (capacity < size) return 0;
  for (size_t i = 0; i < size; ++i) out[i] = 0;
  out[0] = 'L'; out[1] = 'D'; out[2] = 'R'; out[3] = 'V';
  put(out + 4, 4, 2); out[6] = config.count; out[7] = config.gpio_count; put(out + 8, size, 4);
  for (size_t i = 0; i < config.count; ++i) {
    const auto &d = config.drivers[i]; auto *p = out + HEADER_SIZE + i * RECORD_SIZE;
    p[0] = d.id; p[1] = d.type; p[2] = d.data_pin; p[3] = d.clock_pin; p[4] = d.latch_pin; p[5] = d.led_count;
    put(p + 6, d.default_pixels, 2); put(p + 8, d.bcm_base_period_us, 4);
    for (size_t j = 0; j < 2; ++j) {
      p[12 + j * 3] = d.leds[j].red; p[13 + j * 3] = d.leds[j].green; p[14 + j * 3] = d.leds[j].blue;
      if (d.leds[j].active_low) p[18] |= uint8_t(1u << j);
    }
  }
  for (size_t i = 0; i < config.gpio_count; ++i) {
    auto *p = out + HEADER_SIZE + config.count * RECORD_SIZE + i * 2;
    p[0] = config.gpios[i].pin; p[1] = config.gpios[i].pull | (config.gpios[i].inverted ? 0x80 : 0);
  }
  out[size - 1] = config.role;
  put(out + 12, image_checksum(out, size), 4);
  return size;
}
inline bool decode(const uint8_t *data, size_t size, configuration &result) {
  const auto version = size >= HEADER_SIZE ? get(data + 4, 2) : 0;
  const size_t gpio_count = size >= HEADER_SIZE ? data[7] : 0;
  if (size < HEADER_SIZE || data[0] != 'L' || data[1] != 'D' || data[2] != 'R' || data[3] != 'V' ||
      version != 4 || data[6] > MAX_DRIVERS ||
      gpio_count > MAX_GPIO_INPUTS || get(data + 8, 4) != size ||
      size != HEADER_SIZE + data[6] * RECORD_SIZE + gpio_count * 2 + 1 ||
      get(data + 12, 4) != image_checksum(data, size)) return false;
  configuration parsed{}; parsed.count = data[6]; parsed.gpio_count = gpio_count;
  for (size_t i = 0; i < parsed.count; ++i) {
    auto &d = parsed.drivers[i]; const auto *p = data + HEADER_SIZE + i * RECORD_SIZE;
    if ((p[18] & ~3u) || p[19]) return false;
    d.id = p[0]; d.type = p[1]; d.data_pin = p[2]; d.clock_pin = p[3]; d.latch_pin = p[4]; d.led_count = p[5];
    d.default_pixels = get(p + 6, 2); d.bcm_base_period_us = get(p + 8, 4);
    for (size_t j = 0; j < 2; ++j) d.leds[j] = {p[12 + j * 3], p[13 + j * 3], p[14 + j * 3], bool(p[18] & (1u << j))};
  }
  for (size_t i = 0; i < parsed.gpio_count; ++i) {
    const auto *p = data + HEADER_SIZE + parsed.count * RECORD_SIZE + i * 2;
    if ((p[1] & 0x7f) > PULL_DOWN) return false;
    parsed.gpios[i] = {p[0], uint8_t(p[1] & 0x7f), bool(p[1] & 0x80)};
  }
  parsed.role = data[size - 1];
  if (parsed.role != NODE_MASTER && parsed.role != NODE_SLAVE) return false;
  if (!validate(parsed)) return false;
  result = parsed; return true;
}
} // namespace hardware_config
