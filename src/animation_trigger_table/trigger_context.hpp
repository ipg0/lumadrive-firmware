#pragma once

#include <driver/gpio.h>

typedef struct {
  uint8_t from_node_mac[6];
  gpio_num_t gpio_num;
  bool rising;
} trigger_gpio_edge_t;

class trigger_context {
public:
  size_t gpio_edges_count;
  trigger_gpio_edge_t gpio_edges[];

  // TODO: form context & propagate to slave nodes
};
