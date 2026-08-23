#include "ble_interop/ble_interop.hpp"
#include "mesh_interop/mesh_interop.hpp"
#include "global_config/global_config.hpp"
#include "master_clock_manager/master_clock_manager.hpp"
#include <esp_event.h>

extern "C" {
void app_main() {
  global_config::instance();
  master_clock_manager::instance();
  mesh_interop::instance();
  ble_interop::instance();

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
}
