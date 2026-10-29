#pragma once
#include "FreeRTOS.h"
using TaskHandle_t = void *;
inline int xTaskCreate(void (*)(void *), const char *, uint32_t, void *, unsigned, TaskHandle_t *) { return pdPASS; }
inline int xTaskCreatePinnedToCore(void (*)(void *), const char *, uint32_t, void *, unsigned, TaskHandle_t *, int) { return pdPASS; }
#define portNUM_PROCESSORS 2
inline void vTaskNotifyGiveFromISR(TaskHandle_t, BaseType_t *) {}
inline void xTaskNotifyGive(TaskHandle_t) {}
inline unsigned ulTaskNotifyTake(BaseType_t, TickType_t) { return 0; }
inline TickType_t xTaskGetTickCount() { return 0; }
inline void vTaskDelayUntil(TickType_t *, TickType_t) {}
