#include "ota_update.hpp"
#include "mesh_ota.hpp"
#include "global_config/global_config.hpp"
#include "mesh_interop/mesh_interop.hpp"
#include <atomic>
#include <cstring>
#include <mutex>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace ota_update {
namespace {
constexpr uint8_t BEGIN = 1, CHUNK = 2, COMMIT = 3, ABORT = 4, SELECT = 5, REBOOT = 6;
constexpr uint8_t IDLE = 0, RECEIVING = 1, COMPLETE = 2, FAILED = 3, SELECTED = 4;
constexpr uint64_t TIMEOUT_US = 30000000;
struct packet {
  uint32_t generation;
  size_t size;
  uint8_t bytes[512];
};
struct transfer_status {
  uint8_t stage = IDLE, error = 0;
  uint32_t id = 0, offset = 0, total = 0;
};
QueueHandle_t queue = nullptr;
std::atomic<uint32_t> generation{0};
std::mutex status_mutex;
transfer_status published;

uint32_t get32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void put32(uint8_t* p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i)
    p[i] = uint8_t(n >> (8 * i));
}
uint32_t update_crc(uint32_t crc, const uint8_t* p, size_t size) {
  while (size--) {
    crc ^= *p++;
    for (int i = 0; i < 8; ++i)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
  }
  return crc;
}
void publish(const transfer_status& s) {
  std::lock_guard<std::mutex> lock(status_mutex);
  published = s;
}
transfer_status read_status(const uint8_t* bytes) {
  return {bytes[0], bytes[1], get32(bytes + 4), get32(bytes + 8), get32(bytes + 12)};
}
void worker(void*) {
  transfer_status s;
  esp_ota_handle_t handle = 0;
  const esp_partition_t* partition = nullptr;
  uint32_t expected_crc = 0, crc = 0, owner = 0;
  uint64_t touched = 0;
  bool remote = false;
  uint8_t remote_mac[6]{};
  auto abort = [&] {
    if (handle) {
      esp_ota_abort(handle);
      handle = 0;
    }
  };
  auto fail = [&](uint8_t error) {
    abort();
    s.stage = FAILED;
    s.error = error;
    publish(s);
  };
  packet p{};
  for (;;) {
    if (xQueueReceive(queue, &p, pdMS_TO_TICKS(250)) != pdTRUE) {
      if (s.stage == RECEIVING &&
          (owner != generation.load() || uint64_t(esp_timer_get_time()) - touched >= TIMEOUT_US)) {
        if (remote) {
          uint8_t command[5] = {ABORT}, ignored[16];
          put32(command + 1, s.id);
          mesh_ota::exchange(remote_mac, command, sizeof(command), ignored);
          remote = false;
        }
        abort();
        s = {};
        publish(s);
      }
      continue;
    }
    if (p.generation != generation.load())
      continue;
    if (s.stage == RECEIVING && owner != p.generation) {
      abort();
      s = {};
      publish(s);
    }
    const auto* b = p.bytes;
    if (p.size < 5)
      continue;
    const uint32_t id = get32(b + 1);
    if (b[0] == BEGIN && p.size == 19 && id && global_config::instance().is_master_node()) {
      if (s.stage == RECEIVING || !mesh_interop::instance().slave_node_exists(b + 13)) {
        s = {};
        s.id = id;
        fail(9);
        continue;
      }
      std::memcpy(remote_mac, b + 13, 6);
      remote = true;
      owner = p.generation;
      s = {};
      s.id = id;
      s.total = get32(b + 5);
      uint8_t response[16];
      if (!mesh_ota::exchange(remote_mac, b, 13, response)) {
        fail(10);
        remote = false;
      } else {
        s = read_status(response);
        publish(s);
      }
      touched = esp_timer_get_time();
      continue;
    }
    if (remote) {
      if (b[0] == BEGIN && p.size == 13 && s.stage != RECEIVING) {
        remote = false;
        s = {};
      } else {
        if (id != s.id || (b[0] == CHUNK && (p.size < 10 || p.size > 9 + mesh_ota::MAX_CHUNK)))
          continue;
        uint8_t response[16];
        if (!mesh_ota::exchange(remote_mac, b, p.size, response)) {
          fail(10);
          remote = false;
        } else {
          s = read_status(response);
          if (b[0] == REBOOT && s.stage == SELECTED)
            s.stage = 5;
          publish(s);
        }
        touched = esp_timer_get_time();
        if (b[0] == ABORT || b[0] == REBOOT || s.stage == FAILED)
          remote = false;
        continue;
      }
    }
    if (b[0] == BEGIN && p.size == 13 && id) {
      if (s.stage == SELECTED)
        continue;
      if (s.stage == RECEIVING) {
        fail(1);
        continue;
      }
      s = {};
      s.id = id;
      s.total = get32(b + 5);
      expected_crc = get32(b + 9);
      owner = p.generation;
      partition = esp_ota_get_next_update_partition(nullptr);
      if (!partition || !s.total || s.total > partition->size) {
        fail(2);
        continue;
      }
      if (esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
        fail(3);
        continue;
      }
      if (owner != generation.load()) {
        abort();
        s = {};
        publish(s);
        continue;
      }
      crc = 0xffffffffu;
      touched = esp_timer_get_time();
      s.stage = RECEIVING;
      publish(s);
    } else if (id == s.id && b[0] == ABORT && p.size == 5 && s.stage != SELECTED) {
      abort();
      s = {};
      publish(s);
    } else if (id == s.id && s.stage == RECEIVING && owner == p.generation) {
      touched = esp_timer_get_time();
      if (b[0] == CHUNK && p.size > 9) {
        const auto offset = get32(b + 5);
        const auto length = p.size - 9;
        if (offset != s.offset || length > s.total - s.offset) {
          fail(4);
          continue;
        }
        if (esp_ota_write(handle, b + 9, length) != ESP_OK) {
          fail(5);
          continue;
        }
        crc = update_crc(crc, b + 9, length);
        s.offset += length;
        publish(s);
      } else if (b[0] == COMMIT && p.size == 5) {
        if (s.offset != s.total || (crc ^ 0xffffffffu) != expected_crc) {
          fail(6);
          continue;
        }
        const auto result = esp_ota_end(handle);
        handle = 0;
        if (result != ESP_OK) {
          fail(7);
          continue;
        }
        if (owner != generation.load()) {
          s = {};
          publish(s);
          continue;
        }
        s.stage = COMPLETE;
        publish(s);
      }
    } else if (id == s.id && s.stage == COMPLETE && b[0] == SELECT && p.size == 5) {
      if (esp_ota_set_boot_partition(partition) != ESP_OK) {
        fail(8);
        continue;
      }
      s.stage = SELECTED;
      publish(s);
    } else if (id == s.id && s.stage == SELECTED && b[0] == REBOOT && p.size == 5) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      esp_restart();
    }
  }
}
} // namespace

esp_err_t init() {
  queue = xQueueCreate(3, sizeof(packet));
  if (!queue)
    return ESP_ERR_NO_MEM;
  if (xTaskCreate(worker, "ota_update", 4096, nullptr, 4, nullptr) != pdPASS) {
    vQueueDelete(queue);
    queue = nullptr;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
bool submit(const uint8_t* data, size_t size) {
  if (!queue || !data || !size || size > 512)
    return false;
  packet p{};
  p.generation = generation.load();
  p.size = size;
  std::memcpy(p.bytes, data, size);
  return xQueueSend(queue, &p, 0) == pdTRUE;
}
void disconnect() { ++generation; }
void status(uint8_t* out) {
  std::lock_guard<std::mutex> lock(status_mutex);
  std::memset(out, 0, 16);
  out[0] = published.stage;
  out[1] = published.error;
  put32(out + 4, published.id);
  put32(out + 8, published.offset);
  put32(out + 12, published.total);
}
} // namespace ota_update
