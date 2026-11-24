#pragma once
#include <cstddef>
#include <cstdint>
#include <esp_err.h>

namespace mesh_ota {
constexpr size_t MAX_CHUNK = 200;
esp_err_t init();
bool receive(const uint8_t mac[6], const uint8_t* data, size_t size);
bool exchange(const uint8_t mac[6], const uint8_t* data, size_t size, uint8_t status[16]);
} // namespace mesh_ota
