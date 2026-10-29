#pragma once
#include <cstdint>
#include <esp_err.h>

using esp_timer_handle_t = void *;
constexpr int ESP_TIMER_TASK = 0;
struct esp_timer_create_args_t {
  void (*callback)(void *);
  void *arg;
  int dispatch_method;
  const char *name;
  bool skip_unhandled_events;
};
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *handle);
esp_err_t esp_timer_start_once(esp_timer_handle_t handle, uint64_t timeout);
esp_err_t esp_timer_stop(esp_timer_handle_t handle);
esp_err_t esp_timer_delete(esp_timer_handle_t handle);
