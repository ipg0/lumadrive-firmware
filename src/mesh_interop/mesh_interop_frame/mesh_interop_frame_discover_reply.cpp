#include "../../global_config/global_config.hpp"
#include "../mesh_interop.hpp"
#include "mesh_interop_frame.hpp"
#include <esp_log.h>

esp_err_t
mesh_interop_frame<mesh_interop_frame_type::DISCOVER_REPLY>::handle()
    const {
  if (!global_config::instance().is_master_node() ||
      !mesh_interop::instance().is_discovery_mode()) {
    ESP_LOGI(
        "mesh_interop",
        "Received DISCOVER_REPLY frame while not in discover mode or on slave "
        "node, ignoring");
    return ESP_OK;
  }

  if (mesh_interop::instance().slave_node_exists(mac)) {
    ESP_LOGW("mesh_interop",
             "Received DISCOVER_REPLY frame from a slave node that already "
             "exists, re-registering");
  }

  ESP_LOGI("mesh_interop",
           "Received DISCOVER_REPLY frame, replying to frame_id: %u, "
           "negotiating peer MAC: %02X:%02X:%02X:%02X:%02X:%02X",
           this->discover_frame_id, this->mac[0], this->mac[1], this->mac[2],
           this->mac[3], this->mac[4], this->mac[5]);

  mesh_interop_frame<mesh_interop_frame_type::SUBSCRIBE> subscribe_frame(
      this->mac);

  return subscribe_frame.send();
}

mesh_interop_frame<mesh_interop_frame_type::DISCOVER_REPLY>::
    mesh_interop_frame(
        const mesh_interop_frame<mesh_interop_frame_type::DISCOVER>
            *discover_frame)
    : mesh_interop_frame_base(discover_frame->mac) {
  this->discover_frame_id = discover_frame->frame_id;
}

mesh_interop_frame_base::serialized_frame_t *mesh_interop_frame<
    mesh_interop_frame_type::DISCOVER_REPLY>::serialize_payload() const {
  const size_t payload_size = sizeof(this->discover_frame_id);
  auto *frame = reinterpret_cast<serialized_frame_t *>(
      new uint8_t[sizeof(serialized_frame_t) + payload_size]);
  frame->frame_id = this->frame_id;
  frame->magic = mesh_interop::mesh_interop_FRAME_MAGIC;
  frame->frame_type = static_cast<uint8_t>(this->type());
  frame->payload_size = sizeof(this->discover_frame_id);
  memcpy(frame->payload, &this->discover_frame_id,
         sizeof(this->discover_frame_id));
  return frame;
}

void mesh_interop_frame<mesh_interop_frame_type::DISCOVER_REPLY>::
    apply_payload(const uint8_t *payload, size_t payload_size) {
  if (payload_size != sizeof(this->discover_frame_id)) {
    ESP_LOGE("mesh_interop",
             "Invalid payload size for DISCOVER_REPLY frame: %zu",
             payload_size);
    this->discover_frame_id = 0;
    return;
  }
  memcpy(&this->discover_frame_id, payload, sizeof(this->discover_frame_id));
}