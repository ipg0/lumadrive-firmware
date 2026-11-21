#pragma once
#include <cstdint>
#include <esp_err.h>

enum gpio_num_t { GPIO_NUM_5 = 5, GPIO_NUM_18 = 18, GPIO_NUM_19 = 19, GPIO_NUM_23 = 23 };
constexpr int GPIO_MODE_OUTPUT = 1;
constexpr int GPIO_MODE_INPUT = 2;
constexpr int GPIO_PULLDOWN_DISABLE = 0;
constexpr int GPIO_PULLUP_DISABLE = 0;
constexpr int GPIO_PULLDOWN_ENABLE = 1;
constexpr int GPIO_PULLUP_ENABLE = 1;
constexpr int GPIO_INTR_DISABLE = 0;
struct gpio_config_t {
  uint64_t pin_bit_mask;
  int mode;
  int pull_down_en;
  int pull_up_en;
  int intr_type;
};
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
