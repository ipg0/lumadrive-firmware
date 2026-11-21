#pragma once

#include <esp_err.h>
#include <mutex>
#include "esp_mac.h"

class global_config {
public:
  static global_config &instance() {
    static global_config instance;
    return instance;
  }
  inline bool is_master_node() {
    return this->_master_node;
  }
  uint8_t *get_mesh_mac() {
    static uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    return mac;
  }

private:
  static constexpr const char *TAG = "global_config";
  std::mutex _state_mutex;
  bool _master_node;
  global_config();
  ~global_config() = default;
};
