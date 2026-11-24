#include "mesh_ota.hpp"
#include "ota_update.hpp"
#include "global_config/global_config.hpp"
#include "mesh_interop/mesh_interop.hpp"
#include <atomic>
#include <cstring>
#include <esp_now.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace mesh_ota {
namespace {
constexpr uint8_t MAGIC[] = {'L', 'O', 'T', 'A'};
constexpr uint8_t REQUEST = 1, RESPONSE = 2;
constexpr size_t HEADER = 9, MAX_COMMAND = 9 + MAX_CHUNK;
constexpr unsigned RETRIES = 60, RETRY_MS = 250;
struct request {
  uint8_t mac[6];
  size_t size;
  uint8_t bytes[HEADER + MAX_COMMAND];
};
struct response {
  uint8_t mac[6];
  uint32_t sequence;
  uint8_t status[16];
};
QueueHandle_t requests, responses;
std::atomic<uint32_t> next_sequence{0};
uint32_t get32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void put32(uint8_t* p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i)
    p[i] = uint8_t(n >> (i * 8));
}
void send_response(const uint8_t* mac, uint32_t sequence, const uint8_t* status) {
  uint8_t frame[HEADER + 16];
  std::memcpy(frame, MAGIC, 4);
  frame[4] = RESPONSE;
  put32(frame + 5, sequence);
  std::memcpy(frame + HEADER, status, 16);
  esp_now_send(mac, frame, sizeof(frame));
}
bool complete(const uint8_t* command, size_t size, const uint8_t* status) {
  const auto op = command[0];
  const auto id = get32(command + 1);
  const auto stage = status[0];
  const auto status_id = get32(status + 4), offset = get32(status + 8);
  if (op == 4)
    return stage == 0;
  if (status_id != id)
    return false;
  if (stage == 3)
    return true;
  if (op == 1)
    return stage == 1 && offset == 0;
  if (op == 2)
    return size > 9 && stage == 1 && offset == get32(command + 5) + size - 9;
  if (op == 3)
    return stage == 2;
  if (op == 5 || op == 6)
    return stage == 4;
  return false;
}
void worker(void*) {
  request incoming{}, previous{};
  uint8_t cached[16]{};
  bool have_previous = false;
  for (;;) {
    if (xQueueReceive(requests, &incoming, portMAX_DELAY) != pdTRUE)
      continue;
    if (global_config::instance().is_master_node() ||
        !mesh_interop::instance().master_node_matches(incoming.mac))
      continue;
    const auto sequence = get32(incoming.bytes + 5);
    if (have_previous && sequence == get32(previous.bytes + 5) &&
        !std::memcmp(previous.mac, incoming.mac, 6)) {
      if (incoming.size == previous.size &&
          !std::memcmp(incoming.bytes, previous.bytes, incoming.size))
        send_response(incoming.mac, sequence, cached);
      continue;
    }
    const auto* command = incoming.bytes + HEADER;
    const size_t command_size = incoming.size - HEADER;
    if (!ota_update::submit(command, command_size))
      continue;
    const auto started = esp_timer_get_time();
    uint8_t status[16];
    do {
      ota_update::status(status);
      if (complete(command, command_size, status))
        break;
      vTaskDelay(pdMS_TO_TICKS(10));
    } while (esp_timer_get_time() - started < 10000000);
    if (!complete(command, command_size, status))
      continue;
    previous = incoming;
    std::memcpy(cached, status, sizeof(cached));
    have_previous = true;
    send_response(incoming.mac, sequence, status);
  }
}
} // namespace

esp_err_t init() {
  next_sequence.store(esp_random());
  requests = xQueueCreate(4, sizeof(request));
  responses = xQueueCreate(4, sizeof(response));
  if (!requests || !responses)
    return ESP_ERR_NO_MEM;
  return xTaskCreate(worker, "mesh_ota", 4096, nullptr, 4, nullptr) == pdPASS ? ESP_OK
                                                                              : ESP_ERR_NO_MEM;
}
bool receive(const uint8_t mac[6], const uint8_t* data, size_t size) {
  if (size < HEADER || std::memcmp(data, MAGIC, 4))
    return false;
  if (data[4] == REQUEST && size >= HEADER + 5 && size <= HEADER + MAX_COMMAND && requests) {
    request item{};
    std::memcpy(item.mac, mac, 6);
    item.size = size;
    std::memcpy(item.bytes, data, size);
    xQueueSend(requests, &item, 0);
  } else if (data[4] == RESPONSE && size == HEADER + 16 && responses) {
    response item{};
    std::memcpy(item.mac, mac, 6);
    item.sequence = get32(data + 5);
    std::memcpy(item.status, data + HEADER, 16);
    xQueueSend(responses, &item, 0);
  }
  return true;
}
bool exchange(const uint8_t mac[6], const uint8_t* data, size_t size, uint8_t status[16]) {
  if (!responses || size < 5 || size > MAX_COMMAND)
    return false;
  uint8_t frame[HEADER + MAX_COMMAND];
  std::memcpy(frame, MAGIC, 4);
  frame[4] = REQUEST;
  const auto sequence = ++next_sequence;
  put32(frame + 5, sequence);
  std::memcpy(frame + HEADER, data, size);
  response item;
  while (xQueueReceive(responses, &item, 0) == pdTRUE) {
  }
  for (unsigned attempt = 0; attempt < RETRIES; ++attempt) {
    esp_now_send(mac, frame, HEADER + size);
    const auto started = esp_timer_get_time();
    while (esp_timer_get_time() - started < RETRY_MS * 1000) {
      if (xQueueReceive(responses, &item, pdMS_TO_TICKS(RETRY_MS)) != pdTRUE)
        break;
      if (item.sequence == sequence && !std::memcmp(item.mac, mac, 6)) {
        std::memcpy(status, item.status, 16);
        return true;
      }
    }
  }
  return false;
}
} // namespace mesh_ota
