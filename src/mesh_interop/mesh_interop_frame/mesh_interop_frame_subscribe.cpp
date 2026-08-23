#include "../../global_config/global_config.hpp"
#include "../mesh_interop.hpp"
#include "mesh_interop_frame.hpp"
#include <esp_log.h>
#include <esp_now.h>

esp_err_t
mesh_interop_frame<mesh_interop_frame_type::SUBSCRIBE>::handle() const {
  mesh_interop_subscribe_status subscribe_status;

  if (!mesh_interop::instance().is_discovery_mode() ||
      global_config::instance().is_master_node()) {
    ESP_LOGW("mesh_interop",
             "Received SUBSCRIBE frame while not in discover mode or on master "
             "node, rejecting");

    subscribe_status = mesh_interop_subscribe_status::REJECT;
  } else {
    ESP_LOGI("mesh_interop",
             "Received SUBSCRIBE frame with frame_id: %u, subscribing to "
             "master node",
             this->frame_id);

    ESP_ERROR_CHECK(
        mesh_interop::instance().subscribe_to_master_node(this->mac));

    subscribe_status = mesh_interop_subscribe_status::OK;
  }

  const auto *subscribe_reply_frame =
      new mesh_interop_frame<mesh_interop_frame_type::SUBSCRIBE_REPLY>(
          this, subscribe_status);

  esp_err_t ret = subscribe_reply_frame->send();

  delete subscribe_reply_frame;

  return ret;
}
