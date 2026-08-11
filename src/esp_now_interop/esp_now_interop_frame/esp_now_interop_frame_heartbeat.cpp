#include "../../global_config/global_config.hpp"
#include "../esp_now_interop.hpp"
#include "esp_now_interop_frame.hpp"
#include <esp_log.h>

esp_err_t
esp_now_interop_frame<esp_now_interop_frame_type::HEARTBEAT>::handle() const {
  if (global_config::instance().is_master_node() ||
      esp_now_interop::instance().is_discovery_mode()) {
    ESP_LOGI("esp_now_interop", "Received HEARTBEAT frame on master node or in "
                                "discovery node, ignoring");
    return ESP_OK;
  }

  if (!esp_now_interop::instance().master_node_matches(mac)) {
    ESP_LOGW("esp_now_interop",
             "Received HEARTBEAT frame from a master node that does not match "
             "the configured master node, ignoring");
    return ESP_OK;
  }

  // TODO: sync master clock or something

  const auto *heartbeat_reply_frame =
      new esp_now_interop_frame<esp_now_interop_frame_type::HEARTBEAT_REPLY>(
          this);

  esp_err_t ret = heartbeat_reply_frame->send();

  delete heartbeat_reply_frame;
  return ESP_OK;
}

esp_now_interop_frame<esp_now_interop_frame_type::HEARTBEAT>::
    esp_now_interop_frame(const uint8_t mac[ESP_NOW_ETH_ALEN])
    : esp_now_interop_frame_base(mac) {
  this->master_timestamp = esp_timer_get_time();
}

esp_now_interop_frame_base::serialized_frame_t *esp_now_interop_frame<
    esp_now_interop_frame_type::HEARTBEAT>::serialize_payload() const {
  const size_t payload_size = sizeof(this->master_timestamp);
  auto *frame = reinterpret_cast<serialized_frame_t *>(
      new uint8_t[sizeof(serialized_frame_t) + payload_size]);
  frame->frame_id = this->frame_id;
  frame->magic = esp_now_interop::ESP_NOW_INTEROP_FRAME_MAGIC;
  frame->frame_type = static_cast<uint8_t>(this->type());
  frame->payload_size = sizeof(this->master_timestamp);
  memcpy(frame->payload, &this->master_timestamp,
         sizeof(this->master_timestamp));
  return frame;
}

void esp_now_interop_frame<esp_now_interop_frame_type::HEARTBEAT>::
    apply_payload(const uint8_t *payload, size_t payload_size) {
  if (payload_size != sizeof(this->master_timestamp)) {
    ESP_LOGE("esp_now_interop", "Invalid payload size for HEARTBEAT frame: %zu",
             payload_size);
    this->master_timestamp = 0;
    return;
  }
  memcpy(&this->master_timestamp, payload, sizeof(this->master_timestamp));
}