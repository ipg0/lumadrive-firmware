#include "../../global_config/global_config.hpp"
#include "../mesh_interop.hpp"
#include "mesh_interop_frame.hpp"
#include <esp_log.h>
#include <esp_now.h>

esp_err_t
mesh_interop_frame<mesh_interop_frame_type::DISCOVER>::handle() const {
  if (!mesh_interop::instance().is_discovery_mode() ||
      global_config::instance().is_master_node()) {
    ESP_LOGW("mesh_interop",
             "Received DISCOVER frame while not in discover mode or on master "
             "node, ignoring");
    return ESP_OK;
  }

  ESP_LOGI("mesh_interop",
           "Received DISCOVER frame with frame_id: %u, replying",
           this->frame_id);

  const auto *discover_reply_frame =
      new mesh_interop_frame<mesh_interop_frame_type::DISCOVER_REPLY>(
          this);

  esp_err_t ret = discover_reply_frame->send();

  delete discover_reply_frame;

  return ret;
}
