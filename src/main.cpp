#include "ble_interop/ble_interop.hpp"
#include "connected_lighting_drivers/connected_lighting_drivers.hpp"
#include "global_config/global_config.hpp"
#include "master_clock_manager/master_clock_manager.hpp"
#include "mesh_interop/mesh_interop.hpp"
#include <esp_err.h>

extern "C" {
void app_main() {
  global_config::instance();
  master_clock_manager::instance();
  mesh_interop::instance();
  ble_interop::instance();

  ESP_ERROR_CHECK(connected_lighting_drivers::init());
}
}
