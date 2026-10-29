#include "lighting_driver/impl/bcm_74hc595_lighting_driver.hpp"
#include "lighting_driver/impl/linear_lighting_driver_view.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
#include <memory>

namespace {
gptimer_event_callbacks_t timer{};
void *timer_arg = nullptr;
std::vector<uint8_t> latched;
uint8_t shift_register = 0;
uint32_t serial_level = 0;
uint64_t delay_us = 0;
uint64_t now_us = 0, alarm_us = 0;
bool timer_created = false;
bool timer_enabled = false, timer_running = false;
unsigned gpio_config_calls = 0;
unsigned timer_allocations = 0;

hc595_driver_config config() {
  return {
      .ser_pin = GPIO_NUM_23, .srclk_pin = GPIO_NUM_18, .rclk_pin = GPIO_NUM_5,
      .led_count = 2,
      .leds = {{.r_bit = 0, .g_bit = 1, .b_bit = 2, .channels_active_low = false},
               {.r_bit = 4, .g_bit = 5, .b_bit = 6, .channels_active_low = false}},
      .bcm_base_period_us = 39,
  };
}
void tick() {
  assert(timer_created && timer_enabled && timer_running);
  now_us = alarm_us;
  const gptimer_alarm_event_data_t event{now_us, alarm_us};
  assert(!timer.on_alarm(&timer, &event, timer_arg));
}
uint8_t expected(const std::array<color, 2> &pixels, uint8_t brightness, unsigned plane) {
  uint8_t byte = 0;
  for (size_t i = 0; i < pixels.size(); ++i) {
    const uint8_t channels[] = {pixels[i].r, pixels[i].g, pixels[i].b};
    for (unsigned channel = 0; channel < 3; ++channel) {
      const auto level = unsigned(channels[channel]) * brightness / 255;
      if ((level >> plane) & 1) byte |= 1u << (i * 4 + channel);
    }
  }
  return byte;
}
void test_tpic_outputs() {
  bcm_74hc595_lighting_driver driver(config());
  latched.clear();
  assert(driver.init() == ESP_OK && driver._is_ready());
  assert(driver.init() == ESP_ERR_INVALID_STATE);
  assert(latched.size() == 1 && latched.back() == 0 && delay_us == 39);
  const std::array<color, 2> pixels{color(0x81, 0x42, 0x24), color(1, 128, 255)};
  assert(driver.set_pixel(0, alpha_color(pixels[0])) == ESP_OK);
  assert(driver.set_pixel(1, alpha_color(pixels[1])) == ESP_OK);
  assert(driver.set_pixel(2, alpha_color(pixels[0])) == ESP_ERR_INVALID_ARG);
  assert(driver.show() == ESP_OK);
  for (unsigned plane = 0; plane < 8; ++plane) {
    tick();
    assert(latched.back() == expected(pixels, 255, plane));
    assert((latched.back() & 0x88) == 0); // Former COM outputs stay off.
    assert(delay_us == (39u << plane));
  }
  driver.set_global_brightness(128);
  driver.show();
  for (unsigned plane = 0; plane < 8; ++plane) {
    tick(); assert(latched.back() == expected(pixels, 128, plane));
  }
  driver.set_global_brightness(255);
  driver.fill(alpha_color(color(255, 255, 255))); driver.show(); tick(); assert(latched.back() == 0x77);
  driver.clear(); driver.show();
  for (unsigned i = 0; i < 8; ++i) tick();
  assert(latched.back() == 0);
  driver.fill(alpha_color(color(255, 255, 255))); driver.set_global_brightness(0);
  driver.show();
  tick(); assert(latched.back() == 0);
  assert(driver.deinit() == ESP_OK && !driver._is_ready() && !timer_created);
  assert(latched.back() == 0);
}
void test_active_low_external_common() {
  auto cfg = config(); cfg.led_count = 1; cfg.leds[0].channels_active_low = true;
  bcm_74hc595_lighting_driver driver(cfg);
  assert(driver.init() == ESP_OK && latched.back() == 0x07);
  driver.set_pixel(0, alpha_color(color(255, 0, 0))); driver.show(); tick(); assert(latched.back() == 0x06);
  driver.clear(); driver.show();
  for (unsigned i = 0; i < 8; ++i) tick();
  assert(latched.back() == 0x07);
  driver.deinit(); assert(latched.back() == 0x07);
}
void test_mixed_polarity() {
  auto cfg = config(); cfg.leds[0].channels_active_low = true;
  bcm_74hc595_lighting_driver driver(cfg);
  assert(driver.init() == ESP_OK && latched.back() == 0x07);
  driver.fill(alpha_color(color(255, 0, 255))); driver.show(); tick(); assert(latched.back() == 0x52);
  driver.deinit(); assert(latched.back() == 0x07);
}
void test_invalid_mappings() {
  const auto calls = gpio_config_calls;
  for (unsigned invalid = 0; invalid < 6; ++invalid) {
    auto cfg = config();
    switch (invalid) {
    case 0: cfg.led_count = 0; break;
    case 1: cfg.led_count = 3; break;
    case 2: cfg.bcm_base_period_us = 0; break;
    case 3: cfg.leds[0].r_bit = 8; break;
    case 4: cfg.leds[1].r_bit = cfg.leds[0].r_bit; break;
    case 5: cfg.leds[0].g_bit = cfg.leds[0].b_bit; break;
    }
    bcm_74hc595_lighting_driver driver(cfg);
    assert(driver.init() == ESP_ERR_INVALID_ARG && !driver._is_ready());
  }
  assert(gpio_config_calls == calls && !timer_created);
}
void test_frame_publication_and_late_alarm() {
  bcm_74hc595_lighting_driver driver(config());
  assert(driver.show() == ESP_ERR_INVALID_STATE);
  assert(driver.init() == ESP_OK);
  driver.fill(alpha_color(color(255, 255, 255))); driver.show();
  tick(); assert(latched.back() == 0x77);
  // Working clears/partial redraws never reach an in-progress hardware frame.
  driver.clear(); tick(); assert(latched.back() == 0x77);
  driver.set_pixel(0, alpha_color(color(255, 0, 0))); driver.show();
  for (unsigned plane = 2; plane < 8; ++plane) { tick(); assert(latched.back() == 0x77); }
  tick(); assert(latched.back() == 0x01);
  // Simulate a delayed interrupt: rearm in the future relative to the actual
  // latch time instead of scheduling an alarm that is already in the past.
  now_us = alarm_us + 10000;
  const gptimer_alarm_event_data_t event{now_us, alarm_us};
  timer.on_alarm(&timer, &event, timer_arg);
  assert(alarm_us == now_us + 78 && delay_us == 78);
  driver.deinit();
}
void test_faster_bcm_cycle() {
  auto cfg = config(); cfg.bcm_base_period_us = 10;
  bcm_74hc595_lighting_driver driver(cfg);
  assert(driver.init() == ESP_OK);
  driver.fill(alpha_color(color(255, 255, 255))); driver.show();
  uint64_t cycle_us = 0;
  for (unsigned plane = 0; plane < 8; ++plane) {
    tick(); assert(latched.back() == 0x77);
    assert(delay_us == (10u << plane)); cycle_us += delay_us;
  }
  assert(cycle_us == 2550);
  driver.deinit();
}
void test_separate_led_layout_sources() {
  bcm_74hc595_lighting_driver driver(config());
  assert(driver.init() == ESP_OK && driver.get_layout_source_count() == 2);
  linear_lighting_driver_view left(driver, 10, 11, 100, 0), right(driver, 70, 71, 100, 1);
  assert(left.valid() && right.valid());
  assert(!linear_lighting_driver_view(driver, 0, 2, 2).valid());
  assert(!linear_lighting_driver_view(driver, 0, 1, 1, 2).valid());
  left.fill(alpha_color(color(255, 0, 0))); right.fill(alpha_color(color(0, 0, 255)));
  driver.show(); tick(); assert(latched.back() == 0x41);
  left.clear(); driver.show();
  for (unsigned i = 0; i < 8; ++i) tick();
  assert(latched.back() == 0x40); // Clearing LED 0 leaves LED 1's blue intact.
  driver.deinit();
}
void test_six_modules_share_one_timer() {
  std::array<std::unique_ptr<bcm_74hc595_lighting_driver>, 6> drivers;
  const auto allocations = timer_allocations;
  for (size_t i = 0; i < drivers.size(); ++i) {
    drivers[i] = std::make_unique<bcm_74hc595_lighting_driver>(config());
    assert(drivers[i]->init() == ESP_OK);
    drivers[i]->fill(alpha_color(color(255, uint8_t(i * 17), 0))); drivers[i]->show();
  }
  assert(timer_allocations == allocations + 1);
  bcm_74hc595_lighting_driver seventh(config()); assert(seventh.init() == ESP_ERR_NO_MEM);
  auto wrong = config(); wrong.bcm_base_period_us = 10;
  drivers[0]->deinit();
  bcm_74hc595_lighting_driver mismatch(wrong); assert(mismatch.init() == ESP_ERR_INVALID_ARG);
  for (unsigned plane = 0; plane < 8; ++plane) {
    const auto start = latched.size(); tick();
    assert(latched.size() == start + 5);
    for (size_t i = 1; i < drivers.size(); ++i) {
      const color value(255, uint8_t(i * 17), 0);
      assert(latched[start + i - 1] == expected({value, value}, 255, plane));
    }
  }
  for (size_t i = 1; i < drivers.size(); ++i) { assert(timer_created); drivers[i]->deinit(); }
  assert(!timer_created);
}
} // namespace

esp_err_t gpio_config(const gpio_config_t *cfg) {
  ++gpio_config_calls;
  assert(cfg->pin_bit_mask == ((1ULL << 23) | (1ULL << 18) | (1ULL << 5)));
  return ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) {
  if (pin == GPIO_NUM_23) serial_level = level;
  else if (pin == GPIO_NUM_18 && level) shift_register = (shift_register << 1) | serial_level;
  else if (pin == GPIO_NUM_5 && level) latched.push_back(shift_register);
  return ESP_OK;
}
esp_err_t gptimer_new_timer(const gptimer_config_t *cfg, gptimer_handle_t *handle) {
  assert(!timer_created && cfg->resolution_hz == 1000000 && cfg->intr_priority == 3);
  timer_created = true; ++timer_allocations; now_us = 0; *handle = &timer; return ESP_OK;
}
esp_err_t gptimer_register_event_callbacks(gptimer_handle_t handle, const gptimer_event_callbacks_t *callbacks, void *arg) {
  assert(handle == &timer && timer_created); timer = *callbacks; timer_arg = arg; return ESP_OK;
}
esp_err_t gptimer_set_alarm_action(gptimer_handle_t handle, const gptimer_alarm_config_t *alarm) {
  assert(handle == &timer && timer_created && alarm->alarm_count > now_us);
  alarm_us = alarm->alarm_count; delay_us = alarm_us - now_us; return ESP_OK;
}
esp_err_t gptimer_get_raw_count(gptimer_handle_t handle, uint64_t *count) { assert(handle == &timer); *count = now_us; return ESP_OK; }
esp_err_t gptimer_enable(gptimer_handle_t handle) { assert(handle == &timer && !timer_enabled); timer_enabled = true; return ESP_OK; }
esp_err_t gptimer_start(gptimer_handle_t handle) { assert(handle == &timer && timer_enabled); timer_running = true; return ESP_OK; }
esp_err_t gptimer_stop(gptimer_handle_t handle) { assert(handle == &timer && timer_running); timer_running = false; return ESP_OK; }
esp_err_t gptimer_disable(gptimer_handle_t handle) { assert(handle == &timer && !timer_running); timer_enabled = false; return ESP_OK; }
esp_err_t gptimer_del_timer(gptimer_handle_t handle) { assert(handle == &timer && !timer_enabled); timer_created = false; return ESP_OK; }

int main() {
  test_tpic_outputs(); test_active_low_external_common(); test_mixed_polarity(); test_invalid_mappings();
  test_frame_publication_and_late_alarm();
  test_faster_bcm_cycle();
  test_separate_led_layout_sources(); test_six_modules_share_one_timer();
  std::puts("595 driver: external commons, TPIC polarity, BCM planes, brightness, startup/off and mapping validation passed.");
}
