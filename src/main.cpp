#include "ble_interop/ble_interop.hpp"
#include "global_config/global_config.hpp"
#include "lighting_driver/impl/ws2812_lighting_driver.hpp"
#include "master_clock_manager/master_clock_manager.hpp"
#include "mesh_interop/mesh_interop.hpp"
#include <esp_event.h>

namespace {

constexpr TickType_t kAnimationFramePeriodTicks = pdMS_TO_TICKS(10);
constexpr uint32_t kAnimationTaskStackWords = 4096;
constexpr UBaseType_t kAnimationTaskPriority = 5;
constexpr BaseType_t kAnimationTaskCore = 1;

void animation_task(void *param) {
  const ws2812_driver_config ws2812_cfg = {
      .data_pin = GPIO_NUM_19,
      .led_count = 60,
  };

  ws2812_lighting_driver ws2812_driver(ws2812_cfg);
  if (ws2812_driver.init() != ESP_OK) {
    ESP_LOGE("main", "Failed to initialize WS2812 driver");
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI("main", "WS2812 driver initialized successfully");

  color current_color = color(255, 0, 0);
  TickType_t last_wake = xTaskGetTickCount();

  for (;;) {
    current_color = current_color.rotate_hue(1.0f);
    ws2812_driver.fill(alpha_color(current_color));
    ws2812_driver.show();
    vTaskDelayUntil(&last_wake, kAnimationFramePeriodTicks);
  }
}

} // namespace

extern "C" {
void app_main() {
  global_config::instance();
  master_clock_manager::instance();
  mesh_interop::instance();
  ble_interop::instance();

  BaseType_t task_created = xTaskCreatePinnedToCore(
      animation_task, "animation_task", kAnimationTaskStackWords, nullptr,
      kAnimationTaskPriority, nullptr, kAnimationTaskCore);

  if (task_created != pdPASS) {
    ESP_LOGE("main", "Failed to create animation task");
  }
}
}
