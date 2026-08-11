#include "esp_now_interop/esp_now_interop.hpp"
#include "global_config/global_config.hpp"
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <nvs_flash.h>

extern "C" {
void app_main() {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_netif_init());

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  global_config::instance();
  esp_now_interop::instance();

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
}
