#include "ble_interop/ble_interop.hpp"
#include "connected_lighting_drivers/connected_lighting_drivers.hpp"
#include "global_config/global_config.hpp"
#include "master_clock_manager/master_clock_manager.hpp"
#include "mesh_interop/mesh_interop.hpp"
#include "matrix_runtime/matrix_service.hpp"
#include <esp_err.h>

extern "C" {
void app_main() {
  // Clear retained LED frames before initializing storage, networking or mesh.
  ESP_ERROR_CHECK(connected_lighting_drivers::init());
  global_config::instance();
  master_clock_manager::instance();
  mesh_interop::instance();
  ESP_ERROR_CHECK(matrix_service::init());
  ble_interop::instance();
}
}
