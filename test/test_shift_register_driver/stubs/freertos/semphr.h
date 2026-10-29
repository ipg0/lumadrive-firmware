#pragma once
using SemaphoreHandle_t = bool *;
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return new bool(false); }
inline void xSemaphoreGive(SemaphoreHandle_t semaphore) { *semaphore = true; }
inline void xSemaphoreTake(SemaphoreHandle_t, unsigned) {}
inline void vSemaphoreDelete(SemaphoreHandle_t semaphore) { delete semaphore; }
