#include "mesh_interop_frame_heartbeat.hpp"
#include "mesh_interop_frame_heartbeat_reply.hpp"
#include "../../mesh_interop.hpp"
#include "global_config/global_config.hpp"
#include "master_clock_manager/master_clock_manager.hpp"
#include <esp_log.h>
#include <esp_timer.h>

void mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT>::apply_payload(
    const uint8_t *payload, size_t payload_size) {
  if (payload_size != sizeof(_master_timestamp)) {
    ESP_LOGE(TAG, "Invalid payload size for HEARTBEAT frame: %zu", payload_size);
    _master_timestamp = 0;
    return;
  }
  memcpy(&_master_timestamp, payload, sizeof(_master_timestamp));
}

mesh_interop_frame_base::serialized_frame_t *
mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT>::serialize_payload()
    const {
  const size_t payload_size = sizeof(_master_timestamp);
  auto *frame = reinterpret_cast<serialized_frame_t *>(
      new uint8_t[sizeof(serialized_frame_t) + payload_size]);
  frame->frame_id = this->frame_id;
  frame->magic = mesh_interop::mesh_interop_FRAME_MAGIC;
  frame->frame_type = static_cast<uint8_t>(this->type());
  frame->payload_size = sizeof(_master_timestamp);
  memcpy(frame->payload, &_master_timestamp, sizeof(_master_timestamp));
  return frame;
}

mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT>::mesh_interop_frame(
    const uint8_t mac[ESP_NOW_ETH_ALEN])
    : mesh_interop_frame_base(mac) {
  _master_timestamp = esp_timer_get_time();
}

esp_err_t
mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT>::handle() const {
  if (global_config::instance().is_master_node() ||
      mesh_interop::instance().is_discovery_mode()) {
    ESP_LOGI(TAG, "Received HEARTBEAT frame on master node or in "
                  "discovery node, ignoring");
    return ESP_OK;
  }
  if (!mesh_interop::instance().master_node_matches(mac)) {
    ESP_LOGW(TAG, "Received HEARTBEAT frame from a master node that does not "
                  "match the configured master node, ignoring");
    return ESP_OK;
  }
  master_clock_manager::instance().synchronize(_master_timestamp);
  const auto *reply =
      new mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT_REPLY>(this);
  esp_err_t ret = reply->send();
  delete reply;
  return ret;
}
