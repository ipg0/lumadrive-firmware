#pragma once
#include <cstdint>
#include <cstring>
constexpr int ESP_MAC_WIFI_STA = 0;
inline int esp_read_mac(uint8_t *mac, int) {
  const uint8_t value[6] = {2, 1, 2, 3, 4, 5}; memcpy(mac, value, 6); return 0;
}
