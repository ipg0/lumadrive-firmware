#pragma once

#include "../trigger_source.hpp"
#include <cstring>

template <>
class trigger_source<trigger_source_type::GPIO_EDGE>
    : public trigger_source_base {
public:
  trigger_source(const uint8_t from_node_mac[6], gpio_num_t gpio_num, bool rising)
      : _gpio_num(gpio_num), _rising(rising) {
    memcpy(this->_from_node_mac, from_node_mac, 6);
  }
  esp_err_t init() override { return ESP_OK; };
  trigger_source_type type() const override {
    return trigger_source_type::GPIO_EDGE;
  };
  bool poll(const trigger_context &context) override {
    if (context.gpio_edges == nullptr) {
      return false;
    }
    for (size_t i = 0; i < context.gpio_edges_count; i++) {
      const auto &edge = context.gpio_edges[i];
      if (edge.gpio_num == _gpio_num && edge.rising == _rising &&
          memcmp(edge.from_node_mac, this->_from_node_mac, 6) == 0) {
        return true;
      }
    }
    return false;
  };

private:
  uint8_t _from_node_mac[6];
  gpio_num_t _gpio_num;
  bool _rising;
};
