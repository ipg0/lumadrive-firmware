#include "global_config.hpp"
#include "../hardware_config/hardware_config.hpp"
#include "../connected_lighting_drivers/connected_lighting_drivers.hpp"
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <nvs_flash.h>

global_config::global_config() {
  this->_master_node = connected_lighting_drivers::node_role() == hardware_config::NODE_MASTER;

  if (this->_master_node) {
    ESP_LOGI("global_config", "Booting as MASTER");
  } else {
    ESP_LOGI("global_config", "Booting as SLAVE");
  }

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_netif_init());

  ESP_ERROR_CHECK(esp_event_loop_create_default());
}
