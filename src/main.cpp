#include "esp_now_interop/esp_now_interop.hpp"
#include "global_config/global_config.hpp"
#include <esp_event.h>

extern "C" {
void app_main() {
  global_config::instance();
  esp_now_interop::instance();

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
}
