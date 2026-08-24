#pragma once

#include "../mesh_interop/mesh_interop_frame/mesh_interop_frame.hpp"
#include <esp_timer.h>
#include <mutex>

class master_clock_manager {
  friend class mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT>;

public:
  static master_clock_manager &instance() {
    static master_clock_manager instance;
    return instance;
  }

  bool is_synchronized();
  int64_t get_master_time();

protected:
  void synchronize(int64_t);

private:
  static constexpr const char *TAG = "master_clock_manager";
  std::mutex _clock_mutex;
  int64_t _master_clock_offset_us;
  int64_t _last_sync_absolute_time_us;
  bool _synchronized;

  master_clock_manager() : _master_clock_offset_us(0), _synchronized(false) {};
};
