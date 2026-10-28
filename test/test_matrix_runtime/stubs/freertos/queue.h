#pragma once
#include "FreeRTOS.h"
#include <deque>
#include <vector>
#include <cstring>
struct fake_queue { size_t capacity, size; std::deque<std::vector<uint8_t>> items; };
using QueueHandle_t = fake_queue *;
inline QueueHandle_t xQueueCreate(size_t count, size_t size) { return new fake_queue{count, size, {}}; }
inline int xQueueSend(QueueHandle_t q, const void *item, uint32_t) {
  if (!q || q->items.size() == q->capacity) return pdFALSE;
  auto *bytes = static_cast<const uint8_t *>(item);
  q->items.emplace_back(bytes, bytes + q->size); return pdTRUE;
}
inline int xQueueSendFromISR(QueueHandle_t q, const void *item, BaseType_t *) { return xQueueSend(q, item, 0); }
inline int xQueueReceive(QueueHandle_t q, void *item, uint32_t) {
  if (!q || q->items.empty()) return pdFALSE;
  memcpy(item, q->items.front().data(), q->size); q->items.pop_front(); return pdTRUE;
}
