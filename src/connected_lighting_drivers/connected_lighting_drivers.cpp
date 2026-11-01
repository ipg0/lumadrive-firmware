#include "connected_lighting_drivers.hpp"
#include "../lighting_driver/impl/bcm_74hc595_lighting_driver.hpp"
#include "../lighting_driver/impl/ws2812_lighting_driver.hpp"
#include <esp_check.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <iterator>
#include <type_traits>

namespace {

constexpr const char *TAG = "connected_lighting_drivers";

// Connected hardware: update these pins and bit mappings to match the board.
constexpr ws2812_driver_config WS2812_CFG = {
    .data_pin = GPIO_NUM_19,
    .led_count = 60, // default, can be changed in app
};

// TPIC6B595N: LED common anodes connect directly to +12 V. A stored 1 enables
// the corresponding low-side sink. DRAIN3–7 are unused.
constexpr hc595_driver_config HC595_CFG = {
    .ser_pin = GPIO_NUM_23,
    .srclk_pin = GPIO_NUM_18,
    .rclk_pin = GPIO_NUM_5,
    .led_count = 1,
    .leds =
        {
            {.r_bit = 0,
             .g_bit = 1,
             .b_bit = 2,
             .channels_active_low = false},
        },
    // 255 weighted units per cycle: ~392 Hz before ISR/shift overhead.
    .bcm_base_period_us = 10,
};

// Add static instances and register up to six of them below. IDs are stable
// layout references, independent of driver type. Multiple strips/modules work.
ws2812_lighting_driver led_strip_driver(WS2812_CFG);
bcm_74hc595_lighting_driver rgb_module_driver(HC595_CFG);
struct driver_registration {
  uint32_t id;
  rgb_lighting_driver *instance;
  bool (*uses_gpio)(const rgb_lighting_driver *, uint32_t);
  void (*configure_rmt)(rgb_lighting_driver *, size_t);
};
template <typename driver_type>
constexpr driver_registration register_driver(uint32_t id, driver_type &instance) {
  return {id, &instance,
      [](const rgb_lighting_driver *base, uint32_t pin) { return static_cast<const driver_type *>(base)->uses_gpio(pin); },
      [](rgb_lighting_driver *base, size_t symbols) {
        if constexpr (std::is_same_v<driver_type, ws2812_lighting_driver>)
          static_cast<driver_type *>(base)->set_rmt_memory_symbols(symbols);
      }};
}
constexpr driver_registration DRIVERS[] = {
    register_driver(1, led_strip_driver),
    // register_driver(2, rgb_module_driver),
};
static_assert(std::size(DRIVERS) <= connected_lighting_drivers::MAX_DRIVERS, "At most six physical drivers per node");

esp_err_t init_registered_drivers(const driver_registration *entries = DRIVERS, size_t count = std::size(DRIVERS)) {
  if (count > connected_lighting_drivers::MAX_DRIVERS) return ESP_ERR_INVALID_ARG;
  size_t strips = 0;
  for (size_t i = 0; i < count; ++i) {
    const auto &entry = entries[i];
    if (!entry.id || entry.id > connected_lighting_drivers::MAX_DRIVERS) return ESP_ERR_INVALID_ARG;
    if (entry.instance->get_kind() != lighting_driver_kind::LED_STRIP &&
        entry.instance->get_kind() != lighting_driver_kind::RGB_LED_MODULE) return ESP_ERR_INVALID_ARG;
    strips += entry.instance->get_kind() == lighting_driver_kind::LED_STRIP;
    for (size_t j = 0; j < i; ++j) {
      if (entry.id == entries[j].id) return ESP_ERR_INVALID_ARG;
      for (uint32_t pin = 0; pin < 40; ++pin)
        if (entry.uses_gpio(entry.instance, pin) && entries[j].uses_gpio(entries[j].instance, pin)) return ESP_ERR_INVALID_ARG;
    }
  }
  // The ESP32 has eight shared 64-symbol RMT blocks. Keep one strip's full
  // buffer, then divide the bank among all configured strips (up to six).
  const size_t symbols = strips ? (8 / strips) * 64 : 64;
  for (size_t i = 0; i < count; ++i) {
    const auto &entry = entries[i];
    entry.configure_rmt(entry.instance, symbols);
    ESP_RETURN_ON_ERROR(entry.instance->init(), TAG, "Driver %u failed to initialize", unsigned(entry.id));
    // Addressable LEDs retain their last latched frame across an MCU reset.
    ESP_RETURN_ON_ERROR(entry.instance->clear(), TAG, "Driver %u failed to clear", unsigned(entry.id));
    ESP_RETURN_ON_ERROR(entry.instance->show(), TAG, "Driver %u failed to blank", unsigned(entry.id));
  }
  return ESP_OK;
}

} // namespace

namespace connected_lighting_drivers {

bool uses_gpio(uint32_t pin) {
  for (const auto &entry : DRIVERS) if (entry.uses_gpio(entry.instance, pin)) return true;
  return false;
}

rgb_lighting_driver *find(uint32_t id) {
  for (const auto &entry : DRIVERS) if (entry.id == id) return entry.instance;
  return nullptr;
}

esp_err_t init() {
  // RMT binds its interrupt to the CPU that allocates the channel. Keep that
  // allocation on the lighting core; Wi-Fi and BLE run on core 0.
  struct initialization {
    SemaphoreHandle_t complete;
    esp_err_t result = ESP_FAIL;
  } request{xSemaphoreCreateBinary()};
  if (!request.complete) return ESP_ERR_NO_MEM;
  const auto created = xTaskCreatePinnedToCore([](void *arg) {
    auto &request = *static_cast<initialization *>(arg);
    request.result = init_registered_drivers();
    ESP_LOGI(TAG, "Lighting initialized on core %u: %s", unsigned(xPortGetCoreID()), esp_err_to_name(request.result));
    xSemaphoreGive(request.complete);
    vTaskDelete(nullptr);
  }, "lighting_init", 4096, &request, 5, nullptr, portNUM_PROCESSORS - 1);
  if (created != pdPASS) {
    vSemaphoreDelete(request.complete);
    return ESP_ERR_NO_MEM;
  }
  xSemaphoreTake(request.complete, portMAX_DELAY);
  vSemaphoreDelete(request.complete);
  return request.result;
}

} // namespace connected_lighting_drivers
