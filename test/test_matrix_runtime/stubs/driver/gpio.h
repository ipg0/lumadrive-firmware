#pragma once
#include <cstdint>
#include <array>
#include <esp_err.h>
enum gpio_num_t { GPIO_NUM_4 = 4, GPIO_NUM_5 = 5, GPIO_NUM_16 = 16 };
constexpr int GPIO_MODE_INPUT = 1, GPIO_INTR_ANYEDGE = 3;
constexpr int GPIO_PULLUP_ENABLE = 1, GPIO_PULLUP_DISABLE = 0;
constexpr int GPIO_PULLDOWN_ENABLE = 1, GPIO_PULLDOWN_DISABLE = 0;
struct gpio_config_t {
  uint64_t pin_bit_mask = 0;
  int mode = 0, intr_type = 0, pull_up_en = 0, pull_down_en = 0;
};
inline std::array<void (*)(void *), 40> fake_gpio_handlers{};
inline std::array<void *, 40> fake_gpio_args{};
inline std::array<int, 40> fake_gpio_levels{};
inline std::array<gpio_config_t, 40> fake_gpio_configs{};
inline int fake_gpio_failure = -1;
#define GPIO_IS_VALID_GPIO(pin) ((pin) < 40)
inline esp_err_t gpio_install_isr_service(int) { return ESP_OK; }
inline esp_err_t gpio_config(const gpio_config_t *config) {
  for (size_t i = 0; i < 40; ++i) if (config->pin_bit_mask & (1ULL << i)) fake_gpio_configs[i] = *config;
  return ESP_OK;
}
inline esp_err_t gpio_isr_handler_add(gpio_num_t pin, void (*handler)(void *), void *arg) {
  if (int(pin) == fake_gpio_failure) return ESP_FAIL;
  fake_gpio_handlers[pin] = handler; fake_gpio_args[pin] = arg; return ESP_OK;
}
inline esp_err_t gpio_isr_handler_remove(gpio_num_t pin) {
  fake_gpio_handlers[pin] = nullptr; fake_gpio_args[pin] = nullptr; return ESP_OK;
}
inline int gpio_get_level(gpio_num_t pin) { return fake_gpio_levels[pin]; }
