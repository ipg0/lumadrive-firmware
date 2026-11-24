#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wire_encoding {
inline uint64_t get(const uint8_t* data, size_t size) {
  uint64_t value = 0;
  for (size_t i = 0; i < size; ++i)
    value |= uint64_t(data[i]) << (8 * i);
  return value;
}
inline void put(std::vector<uint8_t>& data, uint64_t value, size_t size) {
  for (size_t i = 0; i < size; ++i)
    data.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
} // namespace wire_encoding
