#pragma once

#include <esp_err.h>
#include <mutex>
#define CONFIG_ROLE_GPIO GPIO_NUM_4

class global_config {
public:
  static global_config &instance() {
    static global_config instance;
    return instance;
  }
  inline bool is_master_node() {
    return this->master_node;
  }

private:
  std::mutex state_mutex;
  bool master_node;
  global_config();
  ~global_config() = default;
};
