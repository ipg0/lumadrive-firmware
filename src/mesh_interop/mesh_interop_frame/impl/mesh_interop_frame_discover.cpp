#include "mesh_interop_frame_discover.hpp"
#include "mesh_interop_frame_discover_reply.hpp"
#include "../../mesh_interop.hpp"
#include "global_config/global_config.hpp"
#include <esp_log.h>

esp_err_t
mesh_interop_frame<mesh_interop_frame_type::DISCOVER>::handle() const {
  if (!mesh_interop::instance().is_discovery_mode() ||
      global_config::instance().is_master_node()) {
    ESP_LOGW(TAG, "Received DISCOVER frame while not in discover mode or on "
                  "master node, ignoring");
    return ESP_OK;
  }
  ESP_LOGI(TAG, "Received DISCOVER frame with frame_id: %u, replying",
           this->frame_id);
  const auto *reply =
      new mesh_interop_frame<mesh_interop_frame_type::DISCOVER_REPLY>(this);
  esp_err_t ret = reply->send();
  delete reply;
  return ret;
}
