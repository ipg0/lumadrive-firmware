#include "animation_trigger_table/animation_effect/impl/static_color_animation_effect.hpp"
#include "animation_trigger_table/animation_trigger_table.hpp"
#include "animation_trigger_table/trigger_condition/impl/always_trigger_condition.hpp"
#include "animation_trigger_table/trigger_source/impl/default_state_trigger_source.hpp"
#include "animation_trigger_table/trigger_source/impl/gpio_edge_trigger_source.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>

using static_color =
    animation_effect_descriptor<animation_effect_type::STATIC_COLOR>;
using default_source = trigger_source<trigger_source_type::DEFAULT_STATE>;
using always_condition = trigger_condition<trigger_condition_type::ALWAYS>;

class fake_driver : public rgb_lighting_driver {
public:
  std::array<color, 2> pixels{};
  std::array<color, 2> presented{};
  int clears = 0;
  int shows = 0;
  esp_err_t clear_error = ESP_OK;
  esp_err_t render_error = ESP_OK;
  esp_err_t show_error = ESP_OK;

  esp_err_t init() override { return ESP_OK; }
  esp_err_t deinit() override { return ESP_OK; }
  size_t get_pixel_count() const override { return pixels.size(); }
  esp_err_t set_pixel(uint16_t i, alpha_color value) override {
    if (i >= pixels.size()) {
      return ESP_ERR_INVALID_ARG;
    }
    pixels[i] = value.blend_with(pixels[i]);
    return ESP_OK;
  }
  esp_err_t fill(alpha_color value) override {
    if (render_error != ESP_OK) {
      return render_error;
    }
    for (size_t i = 0; i < pixels.size(); ++i) {
      set_pixel(i, value);
    }
    return ESP_OK;
  }
  esp_err_t clear() override {
    ++clears;
    if (clear_error == ESP_OK) {
      pixels = {};
    }
    return clear_error;
  }
  esp_err_t set_global_brightness(uint8_t) override { return ESP_OK; }
  esp_err_t show() override {
    ++shows;
    presented = pixels;
    return show_error;
  }
  bool _is_ready() const override { return true; }
};

class gate_condition : public trigger_condition_base {
public:
  bool allowed = true;
  trigger_condition_type type() const override {
    return trigger_condition_type::ALWAYS;
  }
  bool evaluate(const trigger_context &) const override { return allowed; }
};

class manual_source : public trigger_source_base {
public:
  bool fired = false;
  esp_err_t init_error = ESP_OK;
  trigger_source_type type() const override {
    return trigger_source_type::GPIO_EDGE;
  }
  esp_err_t init() override { return init_error; }
  bool poll(const trigger_context &) override { return fired; }
};

struct effect_metrics {
  int created = 0;
  int destroyed = 0;
  int starts = 0;
  uint32_t started_at = 0;
};

class counted_instance : public animation_effect_instance_base {
public:
  explicit counted_instance(effect_metrics &metrics) : _metrics(metrics) {
    ++_metrics.created;
  }
  ~counted_instance() override { ++_metrics.destroyed; }
  animation_effect_type type() const override {
    return animation_effect_type::STATIC_COLOR;
  }
  void start(uint32_t now_ms) override {
    ++_metrics.starts;
    _metrics.started_at = now_ms;
    _finished = false;
  }
  void update(uint32_t now_ms) override {
    _finished = static_cast<uint32_t>(now_ms - _metrics.started_at) >= 100;
  }
  bool is_finished() const override { return _finished; }
  esp_err_t render(rgb_lighting_driver &driver) const override {
    return driver.fill(alpha_color(color(255, 0, 0)));
  }

private:
  effect_metrics &_metrics;
  bool _finished = false;
};

class counted_descriptor : public animation_effect_descriptor_base {
public:
  explicit counted_descriptor(effect_metrics &metrics) : _metrics(metrics) {}
  bool fail_allocation = false;
  animation_effect_type type() const override {
    return animation_effect_type::STATIC_COLOR;
  }
  animation_effect_instance_base *create_instance() const override {
    return fail_allocation ? nullptr : new counted_instance(_metrics);
  }

private:
  effect_metrics &_metrics;
};

void expect_color(const fake_driver &driver, uint8_t r, uint8_t g, uint8_t b) {
  for (const auto &pixel : driver.presented) {
    assert(pixel.r == r && pixel.g == g && pixel.b == b);
  }
}

void test_priority_layers_and_frame_reset() {
  fake_driver driver;
  default_source source;
  always_condition condition;
  static_color base(alpha_color(color(255, 0, 0)));
  static_color overlay(alpha_color(color(0, 0, 255), 128));
  animation_trigger_table table;
  // Register the higher priority first to exercise sorting.
  assert(table.add_entry({&overlay, &source, &condition, &driver, 10}) == ESP_OK);
  assert(table.add_entry({&base, &source, &condition, &driver, 0}) == ESP_OK);
  assert(table.tick({}) == ESP_OK);
  expect_color(driver, 127, 0, 128);
  assert(driver.clears == 1 && driver.shows == 1);
  assert(table.tick({}) == ESP_OK);
  expect_color(driver, 127, 0, 128); // No alpha accumulation between frames.
  assert(driver.clears == 2 && driver.shows == 2);
}

void test_equal_priorities_follow_registration_order() {
  fake_driver driver;
  default_source source;
  always_condition condition;
  static_color red(alpha_color(color(255, 0, 0)));
  static_color blue(alpha_color(color(0, 0, 255)));
  static_color transparent(alpha_color(color(0, 255, 0), 0));
  animation_trigger_table table;
  assert(table.add_entry({&red, &source, &condition, &driver, 7}) == ESP_OK);
  assert(table.add_entry({&blue, &source, &condition, &driver, 7}) == ESP_OK);
  assert(table.add_entry({&transparent, &source, &condition, &driver, 255}) ==
         ESP_OK);
  assert(table.tick({}) == ESP_OK);
  expect_color(driver, 0, 0, 255);
}

void test_default_lifecycle_and_condition() {
  fake_driver driver;
  default_source source;
  gate_condition condition;
  effect_metrics metrics;
  counted_descriptor effect(metrics);
  animation_trigger_table table;
  assert(table.add_entry({&effect, &source, &condition, &driver}) == ESP_OK);
  assert(table.tick({.now_ms = 10}) == ESP_OK);
  assert(table.tick({.now_ms = 20}) == ESP_OK);
  assert(metrics.created == 1 && metrics.starts == 1);
  condition.allowed = false;
  assert(table.tick({.now_ms = 30}) == ESP_OK);
  expect_color(driver, 0, 0, 0);
  condition.allowed = true;
  assert(table.tick({.now_ms = 40}) == ESP_OK);
  assert(metrics.created == 1 && metrics.starts == 2);
  expect_color(driver, 255, 0, 0);
  assert(table.tick({.now_ms = 140}) == ESP_OK);
  expect_color(driver, 0, 0, 0);
  assert(table.tick({.now_ms = 141}) == ESP_OK);
  assert(metrics.created == 1 && metrics.starts == 3);
  expect_color(driver, 255, 0, 0);
}

void test_gpio_matching_retrigger_and_completion() {
  const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  trigger_source<trigger_source_type::GPIO_EDGE> source(mac, GPIO_NUM_4, true);
  gate_condition condition;
  fake_driver driver;
  effect_metrics metrics;
  counted_descriptor effect(metrics);
  animation_trigger_table table;
  assert(table.add_entry({&effect, &source, &condition, &driver}) == ESP_OK);
  assert(table.tick({.now_ms = 1}) == ESP_OK);
  assert(metrics.starts == 0);

  trigger_gpio_edge_t edge = {{1, 2, 3, 4, 5, 6}, GPIO_NUM_5, true};
  trigger_context event{.now_ms = 10, .gpio_edges_count = 1, .gpio_edges = &edge};
  assert(table.tick(event) == ESP_OK);
  assert(metrics.starts == 0);
  edge.gpio_num = GPIO_NUM_4;
  edge.rising = false;
  assert(table.tick(event) == ESP_OK);
  assert(metrics.starts == 0);
  edge.rising = true;
  edge.from_node_mac[0] = 9;
  assert(table.tick(event) == ESP_OK);
  assert(metrics.starts == 0);
  edge.from_node_mac[0] = 1;
  condition.allowed = false;
  assert(table.tick(event) == ESP_OK);
  assert(metrics.starts == 0);
  condition.allowed = true;
  assert(table.tick(event) == ESP_OK);
  assert(metrics.starts == 1 && metrics.started_at == 10);
  condition.allowed = false;
  assert(table.tick({.now_ms = 50}) == ESP_OK);
  expect_color(driver, 255, 0, 0); // An accepted event runs to completion.
  condition.allowed = true;
  event.now_ms = 90;
  assert(table.tick(event) == ESP_OK);
  assert(metrics.starts == 2 && metrics.created == 1);
  assert(table.tick({.now_ms = 110}) == ESP_OK);
  expect_color(driver, 255, 0, 0);
  assert(table.tick({.now_ms = 190}) == ESP_OK);
  expect_color(driver, 0, 0, 0);
  assert(table.tick({.now_ms = 200}) == ESP_OK);
  assert(metrics.starts == 2); // No restart without another edge.
}

void test_static_duration_and_clock_wrap() {
  fake_driver driver;
  manual_source source;
  always_condition condition;
  static_color effect(alpha_color(color(0, 255, 0)), 20);
  animation_trigger_table table;
  assert(table.add_entry({&effect, &source, &condition, &driver}) == ESP_OK);
  source.fired = true;
  assert(table.tick({.now_ms = std::numeric_limits<uint32_t>::max() - 9}) ==
         ESP_OK);
  expect_color(driver, 0, 255, 0);
  source.fired = false;
  assert(table.tick({.now_ms = 9}) == ESP_OK);
  expect_color(driver, 0, 255, 0);
  assert(table.tick({.now_ms = 10}) == ESP_OK);
  expect_color(driver, 0, 0, 0);
}

void test_remove_recomposes_and_clear_blanks_all_drivers() {
  fake_driver first, second;
  default_source source;
  always_condition condition;
  static_color red(alpha_color(color(255, 0, 0)));
  static_color blue(alpha_color(color(0, 0, 255)));
  animation_trigger_table table;
  assert(table.add_entry({&red, &source, &condition, &first}) == ESP_OK);
  assert(table.add_entry({&blue, &source, &condition, &first, 10}) == ESP_OK);
  assert(table.add_entry({&blue, &source, &condition, &second}) == ESP_OK);
  assert(table.tick({}) == ESP_OK);
  assert(first.shows == 1 && second.shows == 1);
  assert(table.remove_entry(1) == ESP_OK);
  expect_color(first, 255, 0, 0);
  expect_color(second, 0, 0, 255);
  assert(table.remove_entry(1) == ESP_OK);
  expect_color(second, 0, 0, 0);
  assert(table.clear() == ESP_OK);
  expect_color(first, 0, 0, 0);
  assert(table.size() == 0);
  assert(table.tick({}) == ESP_OK);
  assert(table.remove_entry(0) == ESP_ERR_INVALID_ARG);
}

void test_registration_errors_capacity_and_ownership() {
  fake_driver driver;
  manual_source source;
  always_condition condition;
  effect_metrics metrics;
  counted_descriptor effect(metrics);
  {
    animation_trigger_table table;
    assert(table.add_entry({nullptr, &source, &condition, &driver}) ==
           ESP_ERR_INVALID_ARG);
    assert(table.add_entry({&effect, nullptr, &condition, &driver}) ==
           ESP_ERR_INVALID_ARG);
    assert(table.add_entry({&effect, &source, nullptr, &driver}) ==
           ESP_ERR_INVALID_ARG);
    assert(table.add_entry({&effect, &source, &condition, nullptr}) ==
           ESP_ERR_INVALID_ARG);
    source.init_error = ESP_FAIL;
    assert(table.add_entry({&effect, &source, &condition, &driver}) == ESP_FAIL);
    source.init_error = ESP_OK;
    effect.fail_allocation = true;
    assert(table.add_entry({&effect, &source, &condition, &driver}) ==
           ESP_ERR_NO_MEM);
    assert(table.size() == 0 && metrics.created == 0);
    effect.fail_allocation = false;
    for (size_t i = 0;
         i < animation_trigger_table::ANIMATION_TRIGGER_TABLE_MAX_ENTRIES; ++i) {
      assert(table.add_entry({&effect, &source, &condition, &driver}) == ESP_OK);
    }
    assert(table.add_entry({&effect, &source, &condition, &driver}) ==
           ESP_ERR_NO_MEM);
    assert(table.remove_entry(0) == ESP_OK);
    assert(metrics.destroyed == 1);
    assert(table.add_entry({&effect, &source, &condition, &driver}) == ESP_OK);
    assert(table.clear() == ESP_OK);
    assert(metrics.created == metrics.destroyed);
    assert(table.add_entry({&effect, &source, &condition, &driver}) == ESP_OK);
  }
  assert(metrics.created == metrics.destroyed); // Destruction owns instances.
}

void test_failed_removal_and_clear_can_retry() {
  fake_driver driver;
  default_source source;
  always_condition condition;
  effect_metrics metrics;
  counted_descriptor effect(metrics);
  animation_trigger_table table;
  assert(table.add_entry({&effect, &source, &condition, &driver}) == ESP_OK);
  assert(table.tick({}) == ESP_OK);
  driver.show_error = ESP_FAIL;
  assert(table.remove_entry(0) == ESP_FAIL);
  assert(table.size() == 1 && metrics.destroyed == 0);
  assert(table.clear() == ESP_FAIL);
  assert(table.size() == 1 && metrics.destroyed == 0);
  driver.show_error = ESP_OK;
  assert(table.clear() == ESP_OK);
  assert(table.size() == 0 && metrics.destroyed == 1);
  expect_color(driver, 0, 0, 0);
}

void test_invalid_context_and_driver_errors() {
  fake_driver first, second;
  default_source source;
  always_condition condition;
  static_color effect(alpha_color(color(255, 0, 0)));
  animation_trigger_table table;
  assert(table.add_entry({&effect, &source, &condition, &first}) == ESP_OK);
  assert(table.add_entry({&effect, &source, &condition, &second}) == ESP_OK);
  assert(table.tick({.gpio_edges_count = 1}) == ESP_ERR_INVALID_ARG);
  assert(first.shows == 0);
  first.clear_error = ESP_FAIL;
  assert(table.tick({}) == ESP_FAIL);
  assert(first.shows == 0);
  first.clear_error = ESP_OK;
  first.render_error = ESP_FAIL;
  assert(table.tick({}) == ESP_FAIL);
  assert(first.shows == 0);
  first.render_error = ESP_OK;
  first.show_error = ESP_FAIL;
  assert(table.tick({}) == ESP_FAIL);
  expect_color(second, 255, 0, 0); // Independent outputs are still flushed.
  assert(second.shows == 1);
  first.show_error = ESP_OK;
  assert(table.tick({}) == ESP_OK);
  expect_color(first, 255, 0, 0);
}

int main() {
  test_priority_layers_and_frame_reset();
  test_equal_priorities_follow_registration_order();
  test_default_lifecycle_and_condition();
  test_gpio_matching_retrigger_and_completion();
  test_static_duration_and_clock_wrap();
  test_remove_recomposes_and_clear_blanks_all_drivers();
  test_registration_errors_capacity_and_ownership();
  test_failed_removal_and_clear_can_retry();
  test_invalid_context_and_driver_errors();
  std::puts("All 9 trigger matrix tests passed.");
}
