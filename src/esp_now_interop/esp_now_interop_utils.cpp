#include "esp_now_interop_utils.hpp"
#include <cstring>
#include <esp_now.h>

esp_err_t esp_now_interop_utils::add_peer(const uint8_t mac[6]) {
  esp_now_peer_info_t peer_info = {};
  memset(&peer_info, 0, sizeof(peer_info));
  peer_info.channel = ESP_NOW_CHANNEL;
  peer_info.encrypt = false;
  memcpy(peer_info.peer_addr, mac, ESP_NOW_ETH_ALEN);

  esp_err_t add_peer_err = esp_now_add_peer(&peer_info);
  if (add_peer_err != ESP_OK && add_peer_err != ESP_ERR_ESPNOW_EXIST) {
    return add_peer_err;
  }
  return ESP_OK;
}

esp_err_t esp_now_interop_utils::remove_peer(const uint8_t mac[6]) {
  esp_err_t remove_peer_err = esp_now_del_peer(mac);
  if (remove_peer_err != ESP_OK && remove_peer_err != ESP_ERR_ESPNOW_NOT_FOUND) {
    return remove_peer_err;
  }
  return ESP_OK;
}