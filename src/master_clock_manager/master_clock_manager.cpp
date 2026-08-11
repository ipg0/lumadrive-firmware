#include "master_clock_manager.hpp"
#include "../global_config/global_config.hpp"
#include <esp_log.h>

void master_clock_manager::synchronize(int64_t timestamp_us) {
  std::lock_guard<std::mutex> lock(this->clock_mutex);

  if (global_config::instance().is_master_node()) {
    ESP_LOGE("master_clock_manager",
             "Attempt to synchronize master clock on a master node");
    return;
  }

  if (this->last_sync_absolute_time_us != 0 &&
      timestamp_us < this->last_sync_absolute_time_us) {
    ESP_LOGW("master_clock_manager",
             "Received a timestamp that is earlier than the last sync time, "
             "ignoring");
    return;
  }

  int64_t current_time_us = esp_timer_get_time();

  this->master_clock_offset_us = timestamp_us - current_time_us;
  this->synchronized = true;
  this->last_sync_absolute_time_us = current_time_us;
}

int64_t master_clock_manager::get_master_time() {
  std::lock_guard<std::mutex> lock(this->clock_mutex);

  if (global_config::instance().is_master_node() || !this->synchronized) {
    // master node's own time is the clock
    // if not synchronized, return local time
    return esp_timer_get_time();
  }

  int64_t current_time_us = esp_timer_get_time();
  int64_t master_time_us = current_time_us + this->master_clock_offset_us;
  return master_time_us;
}

bool master_clock_manager::is_synchronized() {
  std::lock_guard<std::mutex> lock(this->clock_mutex);

  if (global_config::instance().is_master_node()) {
    return true; // master node is always synchronized
  }

  return this->synchronized;
}
