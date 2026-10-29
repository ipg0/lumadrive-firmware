#pragma once
#include <cstddef>
#include <cstdint>
#include <driver/gpio.h>
using led_strip_handle_t = void *;
constexpr int LED_MODEL_WS2812 = 0, LED_STRIP_COLOR_COMPONENT_FMT_GRB = 0;
struct led_strip_config_t { int strip_gpio_num; uint32_t max_leds; int led_model, color_component_format; };
struct led_strip_rmt_config_t { uint32_t resolution_hz; size_t mem_block_symbols; };
esp_err_t led_strip_new_rmt_device(const led_strip_config_t *, const led_strip_rmt_config_t *, led_strip_handle_t *);
esp_err_t led_strip_del(led_strip_handle_t);
esp_err_t led_strip_set_pixel(led_strip_handle_t, uint32_t, uint32_t, uint32_t, uint32_t);
esp_err_t led_strip_refresh(led_strip_handle_t);
