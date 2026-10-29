#include "bcm_74hc595_lighting_driver.hpp"
#include <array>

// One hardware timer serves every module. The ESP32 has fewer GPTimers than
// the six driver slots, so modules share the BCM clock and base period.
struct bcm_74hc595_lighting_driver::scheduler {
  gptimer_handle_t timer = nullptr;
  std::array<bcm_74hc595_lighting_driver *, 6> drivers{};
  size_t count = 0;
  uint32_t base_period_us = 0;
  uint8_t plane = 0;
  portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
};
bcm_74hc595_lighting_driver::scheduler bcm_74hc595_lighting_driver::_scheduler;

esp_err_t bcm_74hc595_lighting_driver::init() {
  if (_ready) return ESP_ERR_INVALID_STATE;
  if (!valid_config()) return ESP_ERR_INVALID_ARG;
  auto &clock = _scheduler;
  if (clock.count == clock.drivers.size()) return ESP_ERR_NO_MEM;
  if (clock.count && clock.base_period_us != _cfg.bcm_base_period_us) return ESP_ERR_INVALID_ARG;
  gpio_config_t io_conf = {};
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = (1ULL << _cfg.ser_pin) | (1ULL << _cfg.srclk_pin) | (1ULL << _cfg.rclk_pin);
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  io_conf.intr_type = GPIO_INTR_DISABLE;
  ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "gpio_config failed");
  gpio_set_level(_cfg.ser_pin, 0); gpio_set_level(_cfg.srclk_pin, 0); gpio_set_level(_cfg.rclk_pin, 0);
  shift_out(all_off_byte());
  for (unsigned plane = 0; plane < 8; ++plane) _active_planes[plane] = _pending_planes[plane] = all_off_byte();
  _pending_frame = false;
  const bool first = !clock.timer;
  if (first) {
    gptimer_config_t cfg = {};
    cfg.clk_src = GPTIMER_CLK_SRC_DEFAULT; cfg.direction = GPTIMER_COUNT_UP;
    cfg.resolution_hz = 1000000; cfg.intr_priority = 3;
    ESP_RETURN_ON_ERROR(gptimer_new_timer(&cfg, &clock.timer), TAG, "gptimer_new_timer failed");
    gptimer_event_callbacks_t callbacks = {}; callbacks.on_alarm = bcm_timer_cb;
    auto error = gptimer_register_event_callbacks(clock.timer, &callbacks, nullptr);
    gptimer_alarm_config_t alarm = {}; alarm.alarm_count = _cfg.bcm_base_period_us;
    if (error == ESP_OK) error = gptimer_set_alarm_action(clock.timer, &alarm);
    if (error == ESP_OK) error = gptimer_enable(clock.timer);
    if (error != ESP_OK) { gptimer_del_timer(clock.timer); clock.timer = nullptr; return error; }
    clock.base_period_us = _cfg.bcm_base_period_us; clock.plane = 0;
  }
  portENTER_CRITICAL(&clock.lock);
  clock.drivers[clock.count++] = this; _ready = true;
  portEXIT_CRITICAL(&clock.lock);
  if (first) {
    const auto error = gptimer_start(clock.timer);
    if (error != ESP_OK) {
      _ready = false; clock.count = 0; clock.drivers[0] = nullptr;
      gptimer_disable(clock.timer); gptimer_del_timer(clock.timer); clock.timer = nullptr;
      return error;
    }
  }
  return ESP_OK;
}

esp_err_t bcm_74hc595_lighting_driver::deinit() {
  auto &clock = _scheduler;
  if (_ready) {
    portENTER_CRITICAL(&clock.lock);
    for (size_t i = 0; i < clock.count; ++i) if (clock.drivers[i] == this) {
      for (size_t j = i + 1; j < clock.count; ++j) clock.drivers[j - 1] = clock.drivers[j];
      clock.drivers[--clock.count] = nullptr; break;
    }
    _ready = false;
    portEXIT_CRITICAL(&clock.lock);
    if (!clock.count) {
      gptimer_stop(clock.timer); gptimer_disable(clock.timer); gptimer_del_timer(clock.timer);
      clock.timer = nullptr;
    }
  }
  shift_out(all_off_byte()); return ESP_OK;
}

// Keep ISR routines out of the header: Xtensa needs their literal pools placed
// alongside the IRAM code, rather than in an inline function's COMDAT section.
void bcm_74hc595_lighting_driver::shift_out(uint8_t data) const {
  for (int8_t bit = 7; bit >= 0; --bit) {
    gpio_set_level(_cfg.ser_pin, (data >> bit) & 1u);
    gpio_set_level(_cfg.srclk_pin, 1);
    gpio_set_level(_cfg.srclk_pin, 0);
  }
  gpio_set_level(_cfg.rclk_pin, 1);
  gpio_set_level(_cfg.rclk_pin, 0);
}

bool bcm_74hc595_lighting_driver::bcm_timer_cb(gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *, void *) {
  auto &clock = _scheduler;
  portENTER_CRITICAL_ISR(&clock.lock);
  const uint8_t plane = clock.plane;
  for (size_t driver = 0; driver < clock.count; ++driver) {
    auto *self = clock.drivers[driver];
    if (!plane) {
      portENTER_CRITICAL_ISR(&self->_frame_lock);
      if (self->_pending_frame) {
        for (unsigned i = 0; i < 8; ++i) self->_active_planes[i] = self->_pending_planes[i];
        self->_pending_frame = false;
      }
      portEXIT_CRITICAL_ISR(&self->_frame_lock);
    }
    self->shift_out(self->_active_planes[plane]);
  }
  clock.plane = (plane + 1u) % 8u;
  const auto period = clock.base_period_us;
  portEXIT_CRITICAL_ISR(&clock.lock);
  // Measure the hold from the latch, avoiding callback/shift overhead in the
  // PWM weights and avoiding past alarms if an interrupt was serviced late.
  uint64_t now = 0;
  gptimer_get_raw_count(timer, &now);
  gptimer_alarm_config_t alarm = {};
  alarm.alarm_count = now + (uint64_t(1) << plane) * period;
  gptimer_set_alarm_action(timer, &alarm);
  return false;
}
