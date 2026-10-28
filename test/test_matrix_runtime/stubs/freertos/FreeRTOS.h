#pragma once
#include <cstdint>
using BaseType_t = int;
using TickType_t = uint32_t;
constexpr int pdTRUE = 1, pdFALSE = 0, pdPASS = 1;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(ms) (ms)
#define portYIELD_FROM_ISR() ((void)0)
