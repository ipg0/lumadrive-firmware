#pragma once
#include <cstddef>
#include <cstdint>
#include <esp_err.h>
#define ESP_NOW_ETH_ALEN 6
#define ESP_NOW_MAX_DATA_LEN 250
struct esp_now_recv_info_t {};
esp_err_t esp_now_send(const uint8_t *, const uint8_t *, size_t);
