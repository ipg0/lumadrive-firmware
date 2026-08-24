#include "mesh_interop_frame_subscribe_reply.hpp"
#include "mesh_interop_frame_subscribe.hpp"
#include "../../mesh_interop.hpp"
#include "global_config/global_config.hpp"
#include <esp_log.h>

void mesh_interop_frame<mesh_interop_frame_type::SUBSCRIBE_REPLY>::apply_payload(
    const uint8_t *payload, size_t payload_size) {
  subscribe_reply_payload_t payload_struct;
  if (payload_size != sizeof(payload_struct)) {
    ESP_LOGE(TAG, "Invalid payload size for SUBSCRIBE_REPLY frame: %zu",
             payload_size);
    _subscribe_frame_id = 0;
    _subscribe_status = mesh_interop_subscribe_status::REJECT;
    return;
  }
  memcpy(&payload_struct, payload, sizeof(payload_struct));
  _subscribe_frame_id = payload_struct.subscribe_frame_id;
  _subscribe_status = payload_struct.subscribe_status;
}

mesh_interop_frame_base::serialized_frame_t *
mesh_interop_frame<mesh_interop_frame_type::SUBSCRIBE_REPLY>::serialize_payload()
    const {
  subscribe_reply_payload_t payload;
  const size_t payload_size = sizeof(payload);
  auto *frame = reinterpret_cast<serialized_frame_t *>(
      new uint8_t[sizeof(serialized_frame_t) + payload_size]);
  frame->frame_id = this->frame_id;
  frame->magic = mesh_interop::mesh_interop_FRAME_MAGIC;
  frame->frame_type = static_cast<uint8_t>(this->type());
  frame->payload_size = sizeof(payload);
  payload.subscribe_frame_id = _subscribe_frame_id;
  payload.subscribe_status = _subscribe_status;
  memcpy(frame->payload, &payload, sizeof(payload));
  return frame;
}

// fix: constructor parameter was 'status', not 'subscribe_status'
mesh_interop_frame<mesh_interop_frame_type::SUBSCRIBE_REPLY>::mesh_interop_frame(
    const mesh_interop_frame<mesh_interop_frame_type::SUBSCRIBE> *subscribe_frame,
    mesh_interop_subscribe_status status)
    : mesh_interop_frame_base(subscribe_frame->mac) {
  _subscribe_frame_id = subscribe_frame->frame_id;
  _subscribe_status = status;
}

esp_err_t
mesh_interop_frame<mesh_interop_frame_type::SUBSCRIBE_REPLY>::handle() const {
  if (!global_config::instance().is_master_node() ||
      !mesh_interop::instance().is_discovery_mode()) {
    ESP_LOGI(TAG, "Received SUBSCRIBE_REPLY frame while not in discover mode "
                  "or on slave node, ignoring");
    return ESP_OK;
  }
  ESP_LOGI(TAG,
           "Received SUBSCRIBE_REPLY frame, replying to frame_id: %u "
           "from MAC: %02X:%02X:%02X:%02X:%02X:%02X",
           _subscribe_frame_id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  if (_subscribe_status == mesh_interop_subscribe_status::REJECT) {
    ESP_LOGW(TAG, "Subscription rejected, skipping");
    return ESP_OK;
  }
  ESP_LOGI(TAG, "Subscription accepted, adding slave node");
  return mesh_interop::instance().register_slave_node(mac);
}
