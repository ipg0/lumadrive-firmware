#include "../../global_config/global_config.hpp"
#include "../esp_now_interop.hpp"
#include "esp_now_interop_frame.hpp"
#include <esp_log.h>

esp_err_t
esp_now_interop_frame<esp_now_interop_frame_type::SUBSCRIBE_REPLY>::handle()
    const {
  if (!global_config::instance().is_master_node() ||
      !esp_now_interop::instance().is_discovery_mode()) {
    ESP_LOGI(
        "esp_now_interop",
        "Received SUBSCRIBE_REPLY frame while not in discover mode or on slave "
        "node, ignoring");
    return ESP_OK;
  }

  ESP_LOGI("esp_now_interop",
           "Received SUBSCRIBE_REPLY frame, replying to frame_id: %u "
           "from MAC: %02X:%02X:%02X:%02X:%02X:%02X",
           this->subscribe_frame_id, this->mac[0], this->mac[1], this->mac[2],
           this->mac[3], this->mac[4], this->mac[5]);

  if (this->subscribe_status == esp_now_interop_subscribe_status::REJECT) {
    ESP_LOGW("esp_now_interop", "Subscription rejected, skipping");
    return ESP_OK;
  }

  ESP_LOGI("esp_now_interop", "Subscription accepted, adding slave node");
  esp_err_t ret = esp_now_interop::instance().register_slave_node(this->mac);

  return ret;
}

esp_now_interop_frame<esp_now_interop_frame_type::SUBSCRIBE_REPLY>::
    esp_now_interop_frame(
        const esp_now_interop_frame<esp_now_interop_frame_type::SUBSCRIBE>
            *subscribe_frame,
        esp_now_interop_subscribe_status subscribe_status)
    : esp_now_interop_frame_base(subscribe_frame->mac) {
  this->subscribe_frame_id = subscribe_frame->frame_id;
  this->subscribe_status = subscribe_status;
}

esp_now_interop_frame_base::serialized_frame_t *esp_now_interop_frame<
    esp_now_interop_frame_type::SUBSCRIBE_REPLY>::serialize_payload() const {
  esp_now_interop_frame<esp_now_interop_frame_type::SUBSCRIBE_REPLY>::
      subscribe_reply_payload_t payload;

  const size_t payload_size = sizeof(payload);
  auto *frame = reinterpret_cast<serialized_frame_t *>(
      new uint8_t[sizeof(serialized_frame_t) + payload_size]);
  frame->frame_id = this->frame_id;
  frame->magic = esp_now_interop::ESP_NOW_INTEROP_FRAME_MAGIC;
  frame->frame_type = static_cast<uint8_t>(this->type());
  frame->payload_size = sizeof(payload);

  payload.subscribe_frame_id = this->subscribe_frame_id;
  payload.subscribe_status = this->subscribe_status;

  memcpy(frame->payload, &payload, sizeof(payload));
  return frame;
}

void esp_now_interop_frame<esp_now_interop_frame_type::SUBSCRIBE_REPLY>::
    apply_payload(const uint8_t *payload, size_t payload_size) {
  esp_now_interop_frame<esp_now_interop_frame_type::SUBSCRIBE_REPLY>::
      subscribe_reply_payload_t payload_struct;
  if (payload_size != sizeof(payload_struct)) {
    ESP_LOGE("esp_now_interop",
             "Invalid payload size for SUBSCRIBE_REPLY frame: %zu",
             payload_size);
    this->subscribe_frame_id = 0;
    this->subscribe_status = esp_now_interop_subscribe_status::REJECT;
    return;
  }
  memcpy(&payload_struct, payload, sizeof(payload_struct));
  this->subscribe_frame_id = payload_struct.subscribe_frame_id;
  this->subscribe_status = payload_struct.subscribe_status;
}