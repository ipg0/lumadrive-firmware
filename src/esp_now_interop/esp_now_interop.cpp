#include "esp_now_interop.hpp"
#include "esp_now_interop_frame/esp_now_interop_frame.hpp"
#include <esp_log.h>
#include <esp_wifi.h>

namespace esp_now_interop {

namespace {
const char broadcast_mac[7] = "\xFF\xFF\xFF\xFF\xFF\xFF";

void handle_esp_now_recv(const esp_now_recv_info_t *esp_now_info,
                         const uint8_t *data, int data_len) {
  auto *frame =
      esp_now_interop_frame_base::parse(data, data_len, esp_now_info->src_addr);
  if (frame == nullptr) {
    ESP_LOGE("esp_now_interop", "Failed to parse received frame");
    return;
  }

  frame->handle();

  delete frame;
}
} // namespace

void init() {
  const wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_ERROR_CHECK(esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE));

  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_recv_cb(handle_esp_now_recv));
}

void broadcast_discover() {
  esp_now_peer_info_t peer_info = {};
  memset(&peer_info, 0, sizeof(peer_info));
  peer_info.channel = 6;
  peer_info.encrypt = false;
  memcpy(peer_info.peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);

  esp_err_t add_peer_err = esp_now_add_peer(&peer_info);
  if (add_peer_err != ESP_OK && add_peer_err != ESP_ERR_ESPNOW_EXIST) {
    ESP_LOGE("esp_now_interop", "Failed to add peer: %s",
             esp_err_to_name(add_peer_err));
    return;
  }

  esp_now_interop_frame<esp_now_interop_frame_type::DISCOVER> discover_frame;
  esp_err_t send_err = discover_frame.send();
  if (send_err != ESP_OK) {
    ESP_LOGE("esp_now_interop", "Failed to send discover frame: %s",
             esp_err_to_name(send_err));
  }
}
} // namespace esp_now_interop
