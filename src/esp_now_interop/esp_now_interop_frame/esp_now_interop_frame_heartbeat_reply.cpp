#include "../../global_config/global_config.hpp"
#include "../esp_now_interop.hpp"
#include "esp_now_interop_frame.hpp"
#include <esp_log.h>

esp_err_t
esp_now_interop_frame<esp_now_interop_frame_type::HEARTBEAT_REPLY>::handle()
    const {
  if (!global_config::instance().is_master_node() ||
      esp_now_interop::instance().is_discovery_mode()) {
    ESP_LOGI("esp_now_interop",
             "Received HEARTBEAT_REPLY frame on slave node or in "
             "discovery node, ignoring");
    return ESP_OK;
  }

  return esp_now_interop::instance().sync_slave_node(mac);
}

esp_now_interop_frame<esp_now_interop_frame_type::HEARTBEAT_REPLY>::
    esp_now_interop_frame(
        const esp_now_interop_frame<esp_now_interop_frame_type::HEARTBEAT>
            *heartbeat_frame)
    : esp_now_interop_frame_base(heartbeat_frame->mac) {
  this->heartbeat_frame_id = heartbeat_frame->frame_id;
}

esp_now_interop_frame_base::serialized_frame_t *esp_now_interop_frame<
    esp_now_interop_frame_type::HEARTBEAT_REPLY>::serialize_payload() const {
  const size_t payload_size = sizeof(this->heartbeat_frame_id);
  auto *frame = reinterpret_cast<serialized_frame_t *>(
      new uint8_t[sizeof(serialized_frame_t) + payload_size]);
  frame->frame_id = this->frame_id;
  frame->magic = esp_now_interop::ESP_NOW_INTEROP_FRAME_MAGIC;
  frame->frame_type = static_cast<uint8_t>(this->type());
  frame->payload_size = sizeof(this->heartbeat_frame_id);
  memcpy(frame->payload, &this->heartbeat_frame_id,
         sizeof(this->heartbeat_frame_id));
  return frame;
}

void esp_now_interop_frame<esp_now_interop_frame_type::HEARTBEAT_REPLY>::
    apply_payload(const uint8_t *payload, size_t payload_size) {
  if (payload_size != sizeof(this->heartbeat_frame_id)) {
    ESP_LOGE("esp_now_interop",
             "Invalid payload size for HEARTBEAT_REPLY frame: %zu",
             payload_size);
    this->heartbeat_frame_id = 0;
    return;
  }
  memcpy(&this->heartbeat_frame_id, payload, sizeof(this->heartbeat_frame_id));
}