#include "ble_interop/ble_interop.hpp"
#include "connected_lighting_drivers/connected_lighting_drivers.hpp"
#include "global_config/global_config.hpp"
#include "master_clock_manager/master_clock_manager.hpp"
#include "mesh_interop/mesh_interop.hpp"
#include "mesh_interop/packet_router.hpp"
#include "matrix_runtime/matrix_service.hpp"
#include "hardware_config/hardware_config.hpp"
#include <esp_err.h>
#include <esp_log.h>

extern "C" {
void app_main() {
  // Clear retained LED frames before initializing storage, networking or mesh.
  const auto lighting_status = connected_lighting_drivers::init();
  if (lighting_status != ESP_OK)
    ESP_LOGE("main", "Hardware configuration or lighting initialization failed: %s",
             esp_err_to_name(lighting_status));
  if (connected_lighting_drivers::node_role() == hardware_config::NODE_UNCONFIGURED) {
    ESP_LOGE("main", "Hardware role missing; provision this node over USB");
    return;
  }
  global_config::instance();
  master_clock_manager::instance();
  ESP_ERROR_CHECK(mesh_packet_router().add(matrix_service::receive) ? ESP_OK : ESP_ERR_NO_MEM);
  mesh_interop::instance();
  ESP_ERROR_CHECK(matrix_service::init());
  ble_interop::instance();
}
}
