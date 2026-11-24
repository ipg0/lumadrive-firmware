#include "ota_update/mesh_ota.hpp"
#include "ota_update/ota_update.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>

static int64_t fake_time = 0;
static unsigned sends = 0;
static uint32_t first_sequence = 0;
static bool reply_enabled = true;
int64_t esp_timer_get_time() { return fake_time += 100000; }
namespace ota_update {
bool submit(const uint8_t*, size_t) { return true; }
void status(uint8_t* out) { std::memset(out, 0, 16); }
} // namespace ota_update
esp_err_t esp_now_send(const uint8_t* mac, const uint8_t* frame, size_t size) {
  assert(size <= 250 && frame[4] == 1);
  const uint32_t seq = uint32_t(frame[5]) | uint32_t(frame[6]) << 8 | uint32_t(frame[7]) << 16 |
                       uint32_t(frame[8]) << 24;
  if (!sends)
    first_sequence = seq;
  else
    assert(seq == first_sequence);
  ++sends;
  if (reply_enabled && sends >= 2) {
    uint8_t response[25] = {'L', 'O', 'T', 'A', 2};
    std::memcpy(response + 5, frame + 5, 4);
    response[9] = 1;
    response[13] = 42;
    assert(mesh_ota::receive(mac, response, sizeof(response)));
  }
  return ESP_OK;
}
int main() {
  assert(mesh_ota::init() == ESP_OK);
  const uint8_t mac[] = {2, 0, 0, 0, 0, 1};
  const uint8_t command[] = {1, 42, 0, 0, 0, 10, 0, 0, 0, 0, 0, 0, 0};
  uint8_t status[16]{};
  const uint8_t unrelated[] = {0, 0, 0, 0, 0};
  assert(!mesh_ota::receive(mac, unrelated, sizeof(unrelated)));
  assert(mesh_ota::exchange(mac, command, sizeof(command), status));
  assert(sends == 2 && status[0] == 1 && status[4] == 42);
  assert(!mesh_ota::exchange(mac, command, 4, status));
  std::puts("Mesh OTA: packet routing, bounded frames, and retry acknowledgements passed.");
}
