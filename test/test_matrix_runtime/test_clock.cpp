#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "master_clock_manager/master_clock_manager.hpp"
#include "global_config/global_config.hpp"
#pragma GCC diagnostic pop
#include <cassert>
#include <cstdio>

int64_t now = 500;
int64_t esp_timer_get_time() { return now; }
uint32_t esp_random() { return 100; }
global_config::global_config() : _master_node(false) {}
// Exercise the clock through its existing heartbeat friend interface.
template <> class mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT> {
public:
  static void receive(int64_t timestamp, int64_t arrival, uint64_t session) {
    master_clock_manager::instance().synchronize(timestamp, arrival, session);
  }
};
int main() {
  auto &clock = master_clock_manager::instance();
  using heartbeat = mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT>;
  assert(!clock.is_synchronized()); assert(clock.get_master_time() == 500);
  heartbeat::receive(1000000, 500, 10);
  now = 600; assert(clock.get_master_time() == 1000100);
  assert(clock.get_master_time_at(550) == 1000050); // Queued ISR capture retains its time.
  assert(clock.is_synchronized() && clock.session() == 10);
  heartbeat::receive(990000, 600, 10); // Older master time, despite newer local arrival.
  assert(clock.get_master_time() == 1000100);
  heartbeat::receive(1000, 600, 20); // Master reboot begins a new time domain.
  assert(clock.session() == 20 && clock.get_master_time() == 1000);
  heartbeat::receive(2000000, 700, 10); // Delayed packet from the retired boot.
  assert(clock.session() == 20 && clock.get_master_time() == 1000);
  now = 800; heartbeat::receive(2000, 750, 20);
  assert(clock.get_master_time() == 2050); // Queueing latency does not bias the offset.
  assert(clock.get_master_time_at(775) == 2025);
  std::puts("Clock: initialization, timestamp ordering, boot fencing, and receive-time sync passed.");
}
