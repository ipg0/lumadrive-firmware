#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Registered handlers must only inspect/copy packets; dispatch runs in the radio callback.
class packet_router {
public:
  using handler = bool (*)(const uint8_t*, const uint8_t*, size_t);
  static constexpr size_t MAX_HANDLERS = 4;

  bool add(handler callback) {
    if (!callback)
      return false;
    for (auto& slot : _handlers) {
      handler empty = nullptr;
      if (slot.compare_exchange_strong(empty, callback) || empty == callback)
        return true;
    }
    return false;
  }
  bool dispatch(const uint8_t* mac, const uint8_t* data, size_t size) const {
    for (const auto& slot : _handlers) {
      const auto callback = slot.load();
      if (callback && callback(mac, data, size))
        return true;
    }
    return false;
  }

private:
  std::atomic<handler> _handlers[MAX_HANDLERS]{};
};

inline packet_router& mesh_packet_router() {
  static packet_router router;
  return router;
}
