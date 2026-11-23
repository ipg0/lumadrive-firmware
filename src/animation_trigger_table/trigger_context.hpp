#pragma once

#include <driver/gpio.h>
#include <cstddef>
#include <cstdint>

typedef struct {
  uint8_t from_node_mac[6];
  gpio_num_t gpio_num;
  bool rising;
} trigger_gpio_edge_t;

class trigger_context {
public:
  // Milliseconds on the shared animation clock; unsigned arithmetic wraps safely.
  uint32_t now_ms = 0;
  uint32_t active_mode_id = 0;
  size_t gpio_edges_count = 0;
  // Borrowed for one tick. A null pointer represents an empty event batch.
  const trigger_gpio_edge_t* gpio_edges = nullptr;
  bool (*gpio_level)(void*, const uint8_t*, uint32_t, bool&) = nullptr;
  void* gpio_level_arg = nullptr;
};
