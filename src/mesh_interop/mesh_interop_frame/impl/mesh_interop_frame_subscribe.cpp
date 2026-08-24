#include "mesh_interop_frame_subscribe.hpp"
#include "mesh_interop_frame_subscribe_reply.hpp"
#include "../../mesh_interop.hpp"
#include "global_config/global_config.hpp"
#include <esp_log.h>

esp_err_t
mesh_interop_frame<mesh_interop_frame_type::SUBSCRIBE>::handle() const {
  mesh_interop_subscribe_status subscribe_status;

  if (!mesh_interop::instance().is_discovery_mode() ||
      global_config::instance().is_master_node()) {
    ESP_LOGW(TAG, "Received SUBSCRIBE frame while not in discover mode or on "
                  "master node, rejecting");
    subscribe_status = mesh_interop_subscribe_status::REJECT;
  } else {
    ESP_LOGI(TAG, "Received SUBSCRIBE frame with frame_id: %u, subscribing to "
                  "master node",
             this->frame_id);
    ESP_ERROR_CHECK(
        mesh_interop::instance().subscribe_to_master_node(this->mac));
    subscribe_status = mesh_interop_subscribe_status::OK;
  }

  const auto *reply =
      new mesh_interop_frame<mesh_interop_frame_type::SUBSCRIBE_REPLY>(
          this, subscribe_status);
  esp_err_t ret = reply->send();
  delete reply;
  return ret;
}
