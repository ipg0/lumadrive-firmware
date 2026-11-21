#include "connected_lighting_drivers.hpp"
#include "../hardware_config/hardware_config.hpp"
#include "../lighting_driver/impl/bcm_74hc595_lighting_driver.hpp"
#include "../lighting_driver/impl/ws2812_lighting_driver.hpp"
#include <esp_log.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <array>
#include <memory>
#include <new>

namespace {
constexpr const char *TAG = "connected_lighting_drivers";
using namespace hardware_config;
std::array<std::unique_ptr<rgb_lighting_driver>, MAX_DRIVERS> drivers;
configuration wiring;
esp_err_t initialization_status = ESP_ERR_INVALID_STATE;

esp_err_t read_wiring(configuration &config) {
  const auto *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
      static_cast<esp_partition_subtype_t>(0x40), "hardwarecfg");
  if (!partition || partition->size < HEADER_SIZE) return ESP_ERR_NOT_FOUND;
  std::array<uint8_t, MAX_IMAGE_SIZE> bytes{};
  auto err = esp_partition_read(partition, 0, bytes.data(), HEADER_SIZE);
  if (err != ESP_OK) return err;
  const auto length = get(bytes.data() + 8, 4);
  if (length < HEADER_SIZE || length > bytes.size() || length > partition->size) return ESP_ERR_INVALID_SIZE;
  err = esp_partition_read(partition, HEADER_SIZE, bytes.data() + HEADER_SIZE, length - HEADER_SIZE);
  if (err != ESP_OK) return err;
  return decode(bytes.data(), length, config) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t init_registered_drivers() {
  configuration config{};
  auto err = read_wiring(config);
  if (err != ESP_OK) return err;
  wiring = config;
  size_t strips = 0;
  for (size_t i = 0; i < config.count; ++i) strips += config.drivers[i].type == WS2812;
  const size_t symbols = strips ? (8 / strips) * 64 : 64;
  size_t initialized = 0;
  std::array<uint8_t, MAX_DRIVERS> order{};
  for (size_t i = 0; i < config.count; ++i) {
    const auto &d = config.drivers[i];
    auto &entry = drivers[d.id - 1];
    if (d.type == WS2812) {
      ws2812_driver_config cfg{};
      cfg.data_pin = static_cast<gpio_num_t>(d.data_pin); cfg.led_count = d.default_pixels;
      cfg.rmt_mem_block_symbols = symbols;
      entry.reset(new (std::nothrow) ws2812_lighting_driver(cfg));
    } else {
      hc595_driver_config cfg{};
      cfg.ser_pin = static_cast<gpio_num_t>(d.data_pin); cfg.srclk_pin = static_cast<gpio_num_t>(d.clock_pin);
      cfg.rclk_pin = static_cast<gpio_num_t>(d.latch_pin); cfg.led_count = d.led_count;
      cfg.bcm_base_period_us = d.bcm_base_period_us;
      for (size_t j = 0; j < d.led_count; ++j)
        cfg.leds[j] = {d.leds[j].red, d.leds[j].green, d.leds[j].blue, d.leds[j].active_low};
      entry.reset(new (std::nothrow) bcm_74hc595_lighting_driver(cfg));
    }
    if (!entry) { err = ESP_ERR_NO_MEM; break; }
    err = entry->init();
    if (err != ESP_OK) { entry.reset(); break; }
    order[initialized++] = d.id;
    err = entry->clear();
    if (err == ESP_OK) err = entry->show();
    if (err != ESP_OK) break;
  }
  // Apply the provisioned idle bias even before a matrix uses an input.
  for (size_t i = 0; err == ESP_OK && i < config.gpio_count; ++i) {
    const auto &input = config.gpios[i];
    gpio_config_t cfg{};
    cfg.pin_bit_mask = uint64_t(1) << input.pin;
    cfg.mode = GPIO_MODE_INPUT; cfg.intr_type = GPIO_INTR_DISABLE;
    cfg.pull_up_en = input.pull == PULL_UP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = input.pull == PULL_DOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    err = gpio_config(&cfg);
  }
  if (err != ESP_OK) {
    while (initialized) { auto &d = drivers[order[--initialized] - 1]; d->deinit(); d.reset(); }
    return err;
  }
  return ESP_OK;
}
} // namespace

namespace connected_lighting_drivers {
bool gpio_input(uint32_t pin, uint8_t &pull, bool *inverted) {
  for (size_t i = 0; i < wiring.gpio_count; ++i) {
    if (wiring.gpios[i].pin == pin) {
      pull = wiring.gpios[i].pull;
      if (inverted) *inverted = wiring.gpios[i].inverted;
      return true;
    }
  }
  return false;
}
uint8_t node_role() { return wiring.role; }
bool uses_gpio(uint32_t pin) {
  for (size_t i = 0; i < wiring.count; ++i) {
    const auto &d = wiring.drivers[i];
    if (pin == d.data_pin || (d.type == HC595 && (pin == d.clock_pin || pin == d.latch_pin))) return true;
  }
  return false;
}
rgb_lighting_driver *find(uint32_t id) {
  return id && id <= MAX_DRIVERS ? drivers[id - 1].get() : nullptr;
}
esp_err_t status() { return initialization_status; }
esp_err_t init() {
  struct initialization { SemaphoreHandle_t complete; esp_err_t result = ESP_FAIL; } request{xSemaphoreCreateBinary()};
  if (!request.complete) return ESP_ERR_NO_MEM;
  const auto created = xTaskCreatePinnedToCore([](void *arg) {
    auto &request = *static_cast<initialization *>(arg);
    request.result = init_registered_drivers();
    ESP_LOGI(TAG, "Lighting initialized on core %u: %s", unsigned(xPortGetCoreID()), esp_err_to_name(request.result));
    xSemaphoreGive(request.complete); vTaskDelete(nullptr);
  }, "lighting_init", 4096, &request, 5, nullptr, portNUM_PROCESSORS - 1);
  if (created != pdPASS) { vSemaphoreDelete(request.complete); return ESP_ERR_NO_MEM; }
  xSemaphoreTake(request.complete, portMAX_DELAY);
  vSemaphoreDelete(request.complete);
  initialization_status = request.result;
  return initialization_status;
}
} // namespace connected_lighting_drivers
