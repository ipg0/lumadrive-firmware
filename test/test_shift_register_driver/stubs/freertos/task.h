#pragma once
#include "FreeRTOS.h"
inline int xTaskCreatePinnedToCore(void (*callback)(void *), const char *, unsigned, void *arg, unsigned, void *, int) {
  callback(arg); return pdPASS;
}
inline unsigned xPortGetCoreID() { return 1; }
inline void vTaskDelete(void *) {}
