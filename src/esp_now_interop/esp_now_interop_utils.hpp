#include <cstdint>
#include <esp_err.h>

namespace esp_now_interop_utils {
static constexpr uint32_t ESP_NOW_INTEROP_FRAME_MAGIC = 0xFAB10713;
static constexpr uint8_t ESP_NOW_CHANNEL = 6;

esp_err_t add_peer(const uint8_t mac[6]);
esp_err_t remove_peer(const uint8_t mac[6]);
} // namespace esp_now_interop_utils