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
  uint64_t session();
  int64_t last_sync_at();
  int64_t get_master_time();
  // Convert a captured local timestamp using the current synchronized offset.
  // Call from task context, never from an ISR.
  int64_t get_master_time_at(int64_t local_timestamp_us);

protected:
  void synchronize(int64_t, int64_t received_at_us, uint64_t session);

private:
  static constexpr const char *TAG = "master_clock_manager";
  std::mutex _clock_mutex;
  int64_t _master_clock_offset_us;
  int64_t _last_sync_absolute_time_us;
  int64_t _last_master_timestamp = 0;
  uint64_t _session;
  uint64_t _retired_sessions[4]{};
  size_t _retired_index = 0;
  bool _synchronized;

  master_clock_manager();
};
