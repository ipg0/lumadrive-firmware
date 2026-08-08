#include "../esp_now_interop_utils.hpp"
#include "esp_now_interop_frame.hpp"
#include <esp_log.h>

esp_err_t
esp_now_interop_frame<esp_now_interop_frame_type::DISCOVER_REPLY>::handle()
    const {
  // handle discover reply frame
  ESP_LOGI("esp_now_interop",
           "Received DISCOVER_REPLY frame, replying to frame_id: %u",
           this->discover_frame_id);
  return ESP_OK;
}

esp_now_interop_frame<esp_now_interop_frame_type::DISCOVER_REPLY>::
    esp_now_interop_frame(
        const esp_now_interop_frame<esp_now_interop_frame_type::DISCOVER>
            *discover_frame)
    : esp_now_interop_frame_base(discover_frame->mac) {
  this->discover_frame_id = discover_frame->frame_id;
}

esp_now_interop_frame_base::serialized_frame_t *esp_now_interop_frame<
    esp_now_interop_frame_type::DISCOVER_REPLY>::serialize_payload() const {
  const size_t payload_size = sizeof(this->discover_frame_id);
  auto *frame = reinterpret_cast<serialized_frame_t *>(
      new uint8_t[sizeof(serialized_frame_t) + payload_size]);
  frame->frame_id = this->frame_id;
  frame->magic = esp_now_interop_utils::ESP_NOW_INTEROP_FRAME_MAGIC;
  frame->frame_type = static_cast<uint8_t>(this->type());
  frame->payload_size = sizeof(this->discover_frame_id);
  memcpy(frame->payload, &this->discover_frame_id,
         sizeof(this->discover_frame_id));
  return frame;
}

void esp_now_interop_frame<esp_now_interop_frame_type::DISCOVER_REPLY>::
    apply_payload(const uint8_t *payload, size_t payload_size) {
  if (payload_size != sizeof(this->discover_frame_id)) {
    ESP_LOGE("esp_now_interop",
             "Invalid payload size for DISCOVER_REPLY frame: %zu",
             payload_size);
    this->discover_frame_id = 0;
    return;
  }
  memcpy(&this->discover_frame_id, payload, sizeof(this->discover_frame_id));
}