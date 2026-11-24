#pragma once
#include <freertos/FreeRTOS.h>
using TaskHandle_t = void*;
inline int xTaskCreate(void (*)(void*), const char*, uint32_t, void*, unsigned, TaskHandle_t*) {
  return pdPASS;
}
inline void vTaskDelay(uint32_t) {}
