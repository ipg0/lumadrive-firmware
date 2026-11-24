#pragma once
#include "../mesh_interop/wire_encoding.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace matrix::wire {
constexpr uint32_t MAGIC = 0x5854414d;
constexpr uint8_t VERSION = 0;
constexpr size_t HEADER_SIZE = 8, CHUNK_SIZE = 200, PACKET_LIMIT = 250;
enum type : uint8_t {
  BEGIN = 1,
  CHUNK,
  FINISH,
  ACK,
  COMMAND,
  COMMAND_ACK,
  SNAPSHOT_REQUEST,
  HELLO,
  GPIO_EVENT,
  GPIO_EVENT_ACK,
  INVENTORY_REQUEST,
  INVENTORY,
  IDENTIFY,
  IDENTIFY_ACK,
  GPIO_STATE,
  GPIO_STATE_ACK,
  GPIO_STATE_REQUEST,
  SIGNAL_BATCH,
  SENSOR_BATCH
};
using wire_encoding::get;
using wire_encoding::put;
inline std::vector<uint8_t> packet(type t, const std::vector<uint8_t>& body) {
  if (body.size() + HEADER_SIZE > PACKET_LIMIT)
    return {};
  std::vector<uint8_t> p;
  p.reserve(body.size() + HEADER_SIZE);
  put(p, MAGIC, 4);
  put(p, VERSION, 1);
  put(p, t, 1);
  put(p, body.size(), 2);
  p.insert(p.end(), body.begin(), body.end());
  return p;
}
inline bool valid(const uint8_t* p, size_t n) {
  return n >= HEADER_SIZE && n <= PACKET_LIMIT && get(p, 4) == MAGIC && p[4] == VERSION &&
         get(p + 6, 2) == n - HEADER_SIZE && p[5] >= BEGIN && p[5] <= SENSOR_BATCH;
}
} // namespace matrix::wire
