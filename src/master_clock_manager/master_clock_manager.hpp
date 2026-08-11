#include "../esp_now_interop/esp_now_interop_frame/esp_now_interop_frame.hpp"
#include <esp_timer.h>
#include <mutex>

class master_clock_manager {
  friend class esp_now_interop_frame<esp_now_interop_frame_type::HEARTBEAT>;

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
  std::mutex clock_mutex;
  int64_t master_clock_offset_us;
  int64_t last_sync_absolute_time_us;
  bool synchronized;

  master_clock_manager() : master_clock_offset_us(0), synchronized(false) {};
};
