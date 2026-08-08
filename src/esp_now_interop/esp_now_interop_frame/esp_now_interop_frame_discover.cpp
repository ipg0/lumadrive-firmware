#include "../esp_now_interop_utils.hpp"
#include "esp_now_interop_frame.hpp"
#include <esp_log.h>
#include <esp_now.h>

esp_err_t
esp_now_interop_frame<esp_now_interop_frame_type::DISCOVER>::handle() const {
  // handle discover frame
  ESP_LOGI("esp_now_interop", "Received DISCOVER frame with frame_id: %u",
           this->frame_id);

  bool peer_exists = esp_now_is_peer_exist(this->mac);

  if (!peer_exists) {
    esp_now_interop_utils::add_peer(this->mac);
  }

  const auto *discover_reply_frame =
      new esp_now_interop_frame<esp_now_interop_frame_type::DISCOVER_REPLY>(
          this);

  if (!peer_exists) {
    // clean up if peer added;
    // will re-add if the sender follows up with SUBSCRIBE
    esp_now_interop_utils::remove_peer(this->mac);
  }

  esp_err_t ret = discover_reply_frame->send();

  delete discover_reply_frame;

  return ret;
}
