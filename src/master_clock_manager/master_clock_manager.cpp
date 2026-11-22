#include "master_clock_manager.hpp"
#include "../global_config/global_config.hpp"
#include <esp_log.h>
#include <esp_random.h>

master_clock_manager::master_clock_manager()
    : _master_clock_offset_us(0), _last_sync_absolute_time_us(0),
      _session((uint64_t(esp_random()) << 32) | esp_random()), _synchronized(false) {
  if (!_session) _session = 1;
}

uint64_t master_clock_manager::session() {
  std::lock_guard<std::mutex> lock(_clock_mutex);
  return _session;
}

int64_t master_clock_manager::last_sync_at() {
  std::lock_guard<std::mutex> lock(_clock_mutex);
  return _last_sync_absolute_time_us;
}

void master_clock_manager::synchronize(int64_t timestamp_us, int64_t received_at_us,
                                       uint64_t session) {
  std::lock_guard<std::mutex> lock(this->_clock_mutex);

  if (global_config::instance().is_master_node()) {
    ESP_LOGE("master_clock_manager",
             "Attempt to synchronize master clock on a master node");
    return;
  }

  if (!session || timestamp_us <= 0) return;
  if (session != _session) {
    for (auto retired : _retired_sessions) if (retired == session) return;
    _retired_sessions[_retired_index++ % 4] = _session;
  }
  if (session == _session && timestamp_us <= _last_master_timestamp) {
    ESP_LOGW("master_clock_manager",
             "Received a timestamp that is earlier than the last sync time, "
             "ignoring");
    return;
  }

  int64_t current_time_us = received_at_us ? received_at_us : esp_timer_get_time();

  this->_master_clock_offset_us = timestamp_us - current_time_us;
  this->_synchronized = true;
  this->_last_sync_absolute_time_us = current_time_us;
  _last_master_timestamp = timestamp_us;
  _session = session;
}

int64_t master_clock_manager::get_master_time() {
  return get_master_time_at(esp_timer_get_time());
}

int64_t master_clock_manager::get_master_time_at(int64_t local_timestamp_us) {
  std::lock_guard<std::mutex> lock(this->_clock_mutex);

  if (global_config::instance().is_master_node() || !this->_synchronized) {
    // master node's own time is the clock
    // if not _synchronized, return local time
    return local_timestamp_us;
  }

  int64_t master_time_us = local_timestamp_us + this->_master_clock_offset_us;
  return master_time_us;
}

bool master_clock_manager::is_synchronized() {
  std::lock_guard<std::mutex> lock(this->_clock_mutex);

  if (global_config::instance().is_master_node()) {
    return true; // master node is always _synchronized
  }

  return this->_synchronized;
}
