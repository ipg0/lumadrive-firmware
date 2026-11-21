#pragma once
#include <cstddef>
#include <cstdint>
#include <esp_err.h>

namespace ota_update {
esp_err_t init();
bool submit(const uint8_t *data, size_t size);
void disconnect();
void status(uint8_t *out);
}
