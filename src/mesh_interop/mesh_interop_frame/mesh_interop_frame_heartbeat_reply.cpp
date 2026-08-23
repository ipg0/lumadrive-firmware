#include "../../global_config/global_config.hpp"
#include "../mesh_interop.hpp"
#include "mesh_interop_frame.hpp"
#include <esp_log.h>

esp_err_t
mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT_REPLY>::handle()
    const {
  if (!global_config::instance().is_master_node() ||
      mesh_interop::instance().is_discovery_mode()) {
    ESP_LOGI("mesh_interop",
             "Received HEARTBEAT_REPLY frame on slave node or in "
             "discovery mode, ignoring");
    return ESP_OK;
  }

  return mesh_interop::instance().sync_slave_node(mac);
}

mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT_REPLY>::
    mesh_interop_frame(
        const mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT>
            *heartbeat_frame)
    : mesh_interop_frame_base(heartbeat_frame->mac) {
  this->heartbeat_frame_id = heartbeat_frame->frame_id;
}

mesh_interop_frame_base::serialized_frame_t *mesh_interop_frame<
    mesh_interop_frame_type::HEARTBEAT_REPLY>::serialize_payload() const {
  const size_t payload_size = sizeof(this->heartbeat_frame_id);
  auto *frame = reinterpret_cast<serialized_frame_t *>(
      new uint8_t[sizeof(serialized_frame_t) + payload_size]);
  frame->frame_id = this->frame_id;
  frame->magic = mesh_interop::mesh_interop_FRAME_MAGIC;
  frame->frame_type = static_cast<uint8_t>(this->type());
  frame->payload_size = sizeof(this->heartbeat_frame_id);
  memcpy(frame->payload, &this->heartbeat_frame_id,
         sizeof(this->heartbeat_frame_id));
  return frame;
}

void mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT_REPLY>::
    apply_payload(const uint8_t *payload, size_t payload_size) {
  if (payload_size != sizeof(this->heartbeat_frame_id)) {
    ESP_LOGE("mesh_interop",
             "Invalid payload size for HEARTBEAT_REPLY frame: %zu",
             payload_size);
    this->heartbeat_frame_id = 0;
    return;
  }
  memcpy(&this->heartbeat_frame_id, payload, sizeof(this->heartbeat_frame_id));
}