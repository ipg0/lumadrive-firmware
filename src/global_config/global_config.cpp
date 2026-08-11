#include "global_config.hpp"
#include <driver/gpio.h>
#include <esp_log.h>

global_config::global_config() {
  gpio_config_t role_gpio_conf = {
      .pin_bit_mask = (1ULL << CONFIG_ROLE_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };

  ESP_ERROR_CHECK(gpio_config(&role_gpio_conf));

  int level = gpio_get_level(CONFIG_ROLE_GPIO);

  this->master_node = (level == 0);

  if (this->master_node) {
    ESP_LOGI("global_config", "Booting as MASTER");
  } else {
    ESP_LOGI("global_config", "Booting as SLAVE");
  }
}
