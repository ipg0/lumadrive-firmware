#include "animation_trigger_table/animation_effect/impl/static_color_animation_effect.hpp"
#include "animation_trigger_table/animation_effect/impl/keyframe_animation_effect.hpp"
#include "animation_trigger_table/animation_effect/impl/periodic_animation_effect.hpp"
#include "animation_trigger_table/animation_trigger_table.hpp"
#include "animation_trigger_table/trigger_source/impl/default_state_trigger_source.hpp"
#include "animation_trigger_table/trigger_condition/impl/always_trigger_condition.hpp"
#include "lighting_driver/impl/linear_lighting_driver_view.hpp"
#include <cassert>
#include <cstdio>
#include <memory>
#include <limits>

class animation_test_driver : public rgb_lighting_driver {
public:
  std::array<alpha_color, 4> values{};
  std::array<color, 4> pixels{};
  size_t count = 4;
  size_t center = SIZE_MAX;
  int failure_pixel = -1;
  unsigned clears = 0, shows = 0;
  esp_err_t init() override { return ESP_OK; }
  esp_err_t deinit() override { return ESP_OK; }
  size_t get_pixel_count() const override { return count; }
  size_t get_canvas_center() const override { return center == SIZE_MAX ? count / 2 : center; }
  esp_err_t set_pixel(uint16_t pixel, alpha_color value) override {
    if (pixel == failure_pixel) return ESP_FAIL;
    assert(pixel < count && pixel < pixels.size());
    values[pixel] = value; pixels[pixel] = value.blend_with(pixels[pixel]); return ESP_OK;
  }
  esp_err_t fill(alpha_color value) override {
    for (size_t i = 0; i < count; ++i) {
      auto err = set_pixel(i, value); if (err != ESP_OK) return err;
    }
    return ESP_OK;
  }
  esp_err_t clear() override { pixels = {}; values = {}; ++clears; return ESP_OK; }
  esp_err_t show() override { ++shows; return ESP_OK; }
  esp_err_t set_global_brightness(uint8_t) override { return ESP_OK; }
  bool _is_ready() const override { return true; }
};
using effect_owner = std::unique_ptr<animation_effect_instance_base>;
using keyframe_descriptor = animation_effect_descriptor<animation_effect_type::KEYFRAMES>;

void test_keyframe_segments_and_ownership() {
  animation_keyframe frames[] = {
    {0, alpha_color(color(255, 0, 0))},
    {1000, alpha_color(color(0, 0, 255))},
    {2000, alpha_color(color(0, 255, 0), 128)}
  };
  effect_owner effect;
  {
    keyframe_descriptor descriptor(frames, 3);
    frames[0].value = {}; // The descriptor owns a copy, and so does its instance.
    effect.reset(descriptor.create_instance());
  }
  assert(effect && effect->type() == animation_effect_type::KEYFRAMES);
  animation_test_driver driver;
  effect->start(100); effect->update(600); assert(effect->render(driver) == ESP_OK);
  assert(driver.values[0].base_color.r == 128 && driver.values[0].base_color.b == 128);
  effect->seek(1000); effect->render(driver);
  assert(driver.values[0].base_color.b == 255);
  effect->seek(1500); effect->render(driver);
  assert(driver.values[0].base_color.g == 128 && driver.values[0].alpha == 192);
  effect->seek(UINT64_C(1) << 40); effect->render(driver);
  assert(!effect->is_finished() && driver.values[0].base_color.g == 255 && driver.values[0].alpha == 128);

  frames[0].value = alpha_color(color(255, 0, 0));
  effect.reset(keyframe_descriptor(frames, 3, true).create_instance());
  effect->start(0); effect->seek(2500); effect->render(driver);
  assert(driver.values[0].base_color.r == 128 && driver.values[0].base_color.b == 128);
  effect->seek(2000); effect->render(driver); assert(driver.values[0].base_color.r == 255);
}
void test_keyframe_interpolation_and_validation() {
  animation_test_driver driver;
  animation_keyframe frames[] = {{0, alpha_color(color())}, {1000, alpha_color(color(255, 0, 0))}};
  frames[0].interpolation = animation_interpolation::SMOOTH;
  effect_owner effect(keyframe_descriptor(frames, 2, false, 1500).create_instance());
  effect->start(UINT32_MAX - 100); effect->update(149); effect->render(driver); // 250 ms over wrap.
  assert(driver.values[0].base_color.r == 40);
  effect->seek(750); effect->render(driver); assert(driver.values[0].base_color.r == 215);
  effect->seek(1499); assert(!effect->is_finished()); effect->seek(1500); assert(effect->is_finished());
  effect->start(123); assert(!effect->is_finished()); effect->render(driver);
  assert(driver.values[0].base_color.r == 0);
  frames[0].interpolation = animation_interpolation::STEP;
  effect.reset(keyframe_descriptor(frames, 2).create_instance());
  effect->seek(999); effect->render(driver); assert(driver.values[0].base_color.r == 0);
  effect->seek(1000); effect->render(driver); assert(driver.values[0].base_color.r == 255);
  assert(!keyframe_descriptor(nullptr, 2).create_instance());
  assert(!keyframe_descriptor(frames, 1).create_instance());
  assert(!keyframe_descriptor(frames, 17).create_instance());
  frames[1].time_ms = 0; assert(!keyframe_descriptor(frames, 2).create_instance());
  frames[1].time_ms = 1000; frames[0].time_ms = 1;
  assert(!keyframe_descriptor(frames, 2).create_instance());
  frames[0].time_ms = 0; frames[0].interpolation = static_cast<animation_interpolation>(99);
  assert(!keyframe_descriptor(frames, 2).create_instance());
}
void test_blink_and_breathe() {
  periodic_animation_options options;
  options.primary = alpha_color(color(255, 0, 0), 200); options.duty_percent = 25;
  animation_test_driver driver;
  effect_owner blink(animation_effect_descriptor<animation_effect_type::BLINK>(options).create_instance());
  blink->start(0); blink->seek(249); blink->render(driver); assert(driver.values[0].alpha == 200);
  blink->seek(250); blink->render(driver); assert(driver.values[0].alpha == 0);
  blink->seek(1000); blink->render(driver); assert(driver.values[0].alpha == 200);
  blink->seek(uint64_t(UINT32_MAX) + 256); blink->render(driver); // 4,294,967,551 ms -> off.
  assert(driver.values[0].alpha == 0);
  effect_owner breathe(animation_effect_descriptor<animation_effect_type::BREATHE>(options).create_instance());
  breathe->start(0); breathe->render(driver); assert(driver.values[0].alpha == 0);
  breathe->seek(250); breathe->render(driver); assert(driver.values[0].alpha == 100);
  breathe->seek(500); breathe->render(driver); assert(driver.values[0].alpha == 200);
  breathe->seek(750); breathe->render(driver); assert(driver.values[0].alpha == 100);
  breathe->seek(1000); breathe->render(driver); assert(driver.values[0].alpha == 0);
  options.period_ms = 1;
  assert(!animation_effect_descriptor<animation_effect_type::BLINK>(options).create_instance());
  options.period_ms = 1000; options.duty_percent = 101;
  assert(!animation_effect_descriptor<animation_effect_type::BLINK>(options).create_instance());
}
void test_independent_pixel_tracks() {
  animation_keyframe frames[] = {
    {0, alpha_color(color(255, 0, 0)), animation_interpolation::LINEAR, 0, 0, 1},
    {0, alpha_color(color(0, 255, 0)), animation_interpolation::STEP, 1, 2, 2},
    {1000, alpha_color(color(0, 0, 255)), animation_interpolation::LINEAR, 0, 0, 1},
    {500, alpha_color(color(255, 255, 255), 128), animation_interpolation::LINEAR, 1, 2, 2}
  };
  effect_owner effect(keyframe_descriptor(frames, 4, true).create_instance());
  assert(effect);
  animation_test_driver driver; driver.fill(alpha_color(color(255, 255, 0)));
  effect->seek(250); effect->render(driver);
  assert(driver.values[0].base_color.r == 191 && driver.values[0].base_color.b == 64);
  assert(driver.pixels[1].r == 255 && driver.pixels[1].g == 255); // Unaddressed pixel retains its layer.
  assert(driver.values[2].base_color.g == 255 && driver.values[2].base_color.r == 0);
  assert(driver.values[3].base_color.g == 255 && driver.values[3].base_color.r == 0);
  effect->seek(750); effect->render(driver);
  assert(driver.values[0].base_color.r == 64 && driver.values[0].base_color.b == 191);
  assert(driver.values[2].base_color.r == 255 && driver.values[2].alpha == 128); // Short track holds.
  effect->seek(1000); effect->render(driver);
  assert(driver.values[0].base_color.r == 255 && driver.values[2].base_color.r == 0); // Shared loop boundary.
  driver.count = 3; effect->seek(250); assert(effect->render(driver) == ESP_OK); // Clip ranges to shorter outputs.
  driver.failure_pixel = 2; assert(effect->render(driver) == ESP_FAIL);
  frames[1].wrap = true;
  assert(!keyframe_descriptor(frames, 4).create_instance());
  frames[1].wrap = false; frames[1].time_ms = 1;
  assert(!keyframe_descriptor(frames, 4).create_instance()); // Every track starts at zero.
  frames[1].time_ms = 0;
  assert(!keyframe_descriptor(frames, 3).create_instance()); // Every track has two frames.
  frames[0].all_pixels = true; frames[2].all_pixels = true;
  animation_keyframe full[] = {frames[0], frames[2]};
  effect.reset(keyframe_descriptor(full, 2).create_instance()); assert(effect);
  driver.failure_pixel = -1; effect->seek(1000); effect->render(driver);
  assert(driver.values[1].base_color.b == 255 && driver.values[2].base_color.b == 255);
}
void test_keyframed_motion_width_and_wrapping() {
  animation_keyframe frames[] = {
    {0, alpha_color(color(255, 0, 0)), animation_interpolation::LINEAR, 7, 0, 1},
    {1000, alpha_color(color(255, 0, 0)), animation_interpolation::LINEAR, 7, 3, 1}
  };
  effect_owner effect(keyframe_descriptor(frames, 2).create_instance()); assert(effect);
  animation_test_driver driver;
  effect->seek(500); effect->render(driver); // Position 1.5: smooth coverage across LEDs 1 and 2.
  assert(driver.values[0].alpha == 0 && driver.values[1].alpha == 128 && driver.values[2].alpha == 128);
  assert(driver.values[3].alpha == 0);
  driver.clear(); effect->seek(1000); effect->render(driver);
  assert(driver.values[3].alpha == 255 && driver.values[1].alpha == 0);
  frames[0].wrap = true; frames[1].wrap = true; frames[1].position = 4;
  effect.reset(keyframe_descriptor(frames, 2, true).create_instance()); assert(effect);
  driver.clear(); effect->seek(875); effect->render(driver); // Position 3.5 straddles the strip seam.
  assert(driver.values[3].alpha == 128 && driver.values[0].alpha == 128 && driver.values[1].alpha == 0);
  driver.clear(); effect->seek(1000); effect->render(driver); assert(driver.values[0].alpha == 255);
  frames[0].position = -0.5; frames[1].position = -0.5;
  effect.reset(keyframe_descriptor(frames, 2).create_instance());
  driver.clear(); effect->render(driver); assert(driver.values[3].alpha == 128 && driver.values[0].alpha == 128);
  frames[0].wrap = false; frames[1].wrap = false;
  effect.reset(keyframe_descriptor(frames, 2).create_instance());
  driver.clear(); effect->render(driver); assert(driver.values[3].alpha == 0 && driver.values[0].alpha == 128);
  frames[0].position = 0; frames[1].position = 0; frames[0].width = 0; frames[1].width = 4;
  effect.reset(keyframe_descriptor(frames, 2).create_instance());
  driver.clear(); effect->render(driver); assert(driver.values[0].alpha == 0);
  effect->seek(500); effect->render(driver);
  assert(driver.values[0].alpha == 255 && driver.values[1].alpha == 255 && driver.values[2].alpha == 0);
  frames[0].width = frames[1].width = 1; frames[1].position = 4;
  frames[0].interpolation = animation_interpolation::SMOOTH;
  effect.reset(keyframe_descriptor(frames, 2).create_instance());
  driver.clear(); effect->seek(250); effect->render(driver);
  assert(driver.values[0].alpha == 96 && driver.values[1].alpha == 159);
  frames[0].width = frames[1].width = 8; frames[0].wrap = frames[1].wrap = true;
  effect.reset(keyframe_descriptor(frames, 2).create_instance());
  driver.clear(); effect->render(driver);
  for (const auto &value : driver.values) assert(value.alpha == 255);
  frames[0].width = -1; assert(!keyframe_descriptor(frames, 2).create_instance());
  frames[0].width = 1; frames[0].position = std::numeric_limits<float>::quiet_NaN();
  assert(!keyframe_descriptor(frames, 2).create_instance());
  frames[0].position = 0; frames[0].width = std::numeric_limits<float>::infinity();
  assert(!keyframe_descriptor(frames, 2).create_instance());

  animation_keyframe overlap[] = {
    {0, alpha_color(color(255, 0, 0)), animation_interpolation::LINEAR, 0},
    {1000, alpha_color(color(255, 0, 0)), animation_interpolation::LINEAR, 0},
    {0, alpha_color(color(0, 0, 255), 128), animation_interpolation::LINEAR, 1},
    {1000, alpha_color(color(0, 0, 255), 128), animation_interpolation::LINEAR, 1}
  };
  effect.reset(keyframe_descriptor(overlap, 4).create_instance()); assert(effect);
  driver.clear(); effect->render(driver);
  assert(driver.pixels[0].r == 127 && driver.pixels[0].b == 128);
}
void test_flowing_gradient() {
  periodic_animation_options options;
  options.primary = alpha_color(color(255, 0, 0));
  options.secondary = alpha_color(color(0, 0, 255));
  animation_test_driver driver;
  effect_owner gradient(animation_effect_descriptor<animation_effect_type::FLOWING_GRADIENT>(options).create_instance());
  gradient->start(0); assert(gradient->render(driver) == ESP_OK);
  assert(driver.values[0].base_color.r == 255 && driver.values[2].base_color.b == 255);
  gradient->seek(250); gradient->render(driver);
  assert(driver.values[1].base_color.r == 255 && driver.values[3].base_color.b == 255);
  options.reverse = true;
  gradient.reset(animation_effect_descriptor<animation_effect_type::FLOWING_GRADIENT>(options).create_instance());
  gradient->seek(250); gradient->render(driver);
  assert(driver.values[3].base_color.r == 255 && driver.values[1].base_color.b == 255);
  options.wavelength_pixels = 2;
  gradient.reset(animation_effect_descriptor<animation_effect_type::FLOWING_GRADIENT>(options).create_instance());
  gradient->render(driver); assert(driver.values[0].base_color.r == 255 && driver.values[1].base_color.b == 255);
  driver.failure_pixel = 1; assert(gradient->render(driver) == ESP_FAIL);
  driver.failure_pixel = -1; driver.count = 0; assert(gradient->render(driver) == ESP_OK);
  driver.count = 1; assert(gradient->render(driver) == ESP_OK);
  driver.count = 65537; assert(gradient->render(driver) == ESP_ERR_INVALID_ARG);
}
void test_animated_layers_blend_once_per_frame() {
  animation_test_driver driver;
  trigger_source<trigger_source_type::DEFAULT_STATE> source;
  trigger_condition<trigger_condition_type::ALWAYS> condition;
  animation_effect_descriptor<animation_effect_type::STATIC_COLOR> base(alpha_color(color(255, 0, 0)));
  periodic_animation_options options; options.primary = alpha_color(color(0, 0, 255));
  animation_effect_descriptor<animation_effect_type::BREATHE> overlay(options);
  animation_trigger_table table;
  assert(table.add_entry({&overlay, &source, &condition, &driver, 10}) == ESP_OK);
  assert(table.add_entry({&base, &source, &condition, &driver, 0}) == ESP_OK);
  trigger_context context; assert(table.tick(context) == ESP_OK);
  context.now_ms = 250; assert(table.tick(context) == ESP_OK);
  assert(driver.pixels[0].r == 127 && driver.pixels[0].b == 128);
  assert(driver.clears == 2 && driver.shows == 2);
  assert(table.tick(context) == ESP_OK);
  assert(driver.pixels[0].r == 127 && driver.pixels[0].b == 128);
}
void test_linear_canvas_views() {
  animation_test_driver strip, block; block.count = 2;
  linear_lighting_driver_view strip_view(strip, 0, 4, 6), block_view(block, 3, 1, 6);
  assert(strip_view.valid() && block_view.valid());
  assert(block_view.get_pixel_count() == 6 && block_view.get_render_pixel_count() == 2);
  assert(block_view.get_render_pixel_index(0) == 2 && block_view.get_render_pixel_index(1) == 1);
  periodic_animation_options options;
  options.primary = alpha_color(color(255, 0, 0)); options.secondary = alpha_color(color(0, 0, 255));
  effect_owner effect(animation_effect_descriptor<animation_effect_type::FLOWING_GRADIENT>(options).create_instance());
  effect->render(strip_view); effect->render(block_view);
  assert(block.values[0].base_color.r == strip.values[2].base_color.r);
  assert(block.values[1].base_color.b == strip.values[1].base_color.b);
  assert(strip.values[3].base_color.b == 255); // Wavelength is the global six-pixel extent.
  assert(block.shows == 0 && strip.shows == 0); // Views never present extra frames during rendering.
  const auto before = block.pixels;
  assert(block_view.set_pixel(5, alpha_color(color(255, 255, 255))) == ESP_OK);
  assert(block.pixels[0].r == before[0].r && block.pixels[1].r == before[1].r);
  assert(block_view.set_pixel(6, {}) == ESP_ERR_INVALID_ARG);
  linear_lighting_driver_view invalid(block, 1, 1, 6), wrong_length(block, 0, 3, 6), outside(block, 5, 7, 6);
  assert(!invalid.valid() && !wrong_length.valid() && !outside.valid());
  assert(invalid.fill({}) == ESP_ERR_INVALID_ARG);
  linear_lighting_driver_view high(block, 65536, 65534, 65536);
  assert(high.valid() && high.get_render_pixel_index(0) == 65535);
  animation_keyframe frames[] = {
    {0, alpha_color(color(255, 0, 0)), animation_interpolation::LINEAR, 1, 65535},
    {1000, alpha_color(color(255, 0, 0)), animation_interpolation::LINEAR, 1, 65535}
  };
  effect.reset(keyframe_descriptor(frames, 2).create_instance());
  block.clear(); assert(effect->render(high) == ESP_OK);
  assert(block.pixels[0].r == 255 && block.pixels[1].r == 0);
}
void test_rainbow_zero_offset_views() {
  animation_test_driver strip, block; block.count = 1;
  linear_lighting_driver_view strip_view(strip, 0, 4, 4), block_view(block, 0, 1, 4);
  periodic_animation_options options; options.primary.alpha = 128; options.period_ms = 8000;
  effect_owner effect(animation_effect_descriptor<animation_effect_type::RAINBOW>(options).create_instance());
  assert(effect && effect->type() == animation_effect_type::RAINBOW);
  assert(effect->render(strip_view) == ESP_OK && effect->render(block_view) == ESP_OK);
  assert(strip.values[0].base_color.r == 255 && strip.values[0].base_color.g == 0);
  assert(strip.values[2].base_color.g == 255 && strip.values[2].base_color.b == 255);
  assert(block.values[0].base_color.r == strip.values[0].base_color.r && block.values[0].alpha == 128);
  effect->seek(2000); effect->render(strip_view); effect->render(block_view);
  assert(strip.values[0].base_color.b == 255 && strip.values[0].base_color.r == 128);
  assert(block.values[0].base_color.r == strip.values[0].base_color.r);
  options.reverse = true;
  effect.reset(animation_effect_descriptor<animation_effect_type::RAINBOW>(options).create_instance());
  effect->seek(2000); effect->render(strip_view);
  assert(strip.values[0].base_color.g == 255 && strip.values[0].base_color.b == 0);
  effect->seek(UINT64_C(1) << 40); assert(effect->render(strip_view) == ESP_OK && !effect->is_finished());
}
void test_multi_color_gradient() {
  periodic_animation_options options;
  options.period_ms = 1000; options.wavelength_pixels = 4;
  options.gradient_color_count = 4;
  options.gradient_colors[0] = alpha_color(color(255, 0, 0), 255);
  options.gradient_colors[1] = alpha_color(color(0, 255, 0), 128);
  options.gradient_colors[2] = alpha_color(color(0, 0, 255), 0);
  options.gradient_colors[3] = alpha_color(color(255, 255, 255), 255);
  using descriptor = animation_effect_descriptor<animation_effect_type::FLOWING_GRADIENT>;
  effect_owner effect(descriptor(options).create_instance());
  assert(effect);
  animation_test_driver driver;
  assert(effect->render(driver) == ESP_OK);
  assert(driver.values[0].base_color.r == 255 && driver.values[1].base_color.g == 255);
  assert(driver.values[2].base_color.b == 255 && driver.values[2].alpha == 0);
  assert(driver.values[3].base_color.r == 255 && driver.values[3].base_color.g == 255);
  effect->seek(125); effect->render(driver); // Seam interpolates last stop back to first.
  assert(driver.values[0].base_color.r == 255 && driver.values[0].base_color.g == 128);
  assert(driver.values[1].base_color.r == 128 && driver.values[1].base_color.g == 128);
  assert(driver.values[1].alpha == 192);
  options.reverse = true;
  effect.reset(descriptor(options).create_instance()); effect->seek(250); effect->render(driver);
  assert(driver.values[0].base_color.g == 255 && driver.values[0].alpha == 128);
  options.gradient_color_count = 1; assert(!descriptor(options).create_instance());
  options.gradient_color_count = 9; assert(!descriptor(options).create_instance());
  options.gradient_color_count = 8; effect.reset(descriptor(options).create_instance()); assert(effect);
}
void test_fill_effect() {
  periodic_animation_options options; options.primary = alpha_color(color(255, 0, 0), 128);
  options.period_ms = 1000; options.duration_ms = 1000;
  using descriptor = animation_effect_descriptor<animation_effect_type::FILL>;
  effect_owner effect(descriptor(options).create_instance()); assert(effect);
  animation_test_driver driver;
  effect->seek(0); effect->render(driver); assert(driver.values[0].alpha == 0);
  effect->seek(250); effect->render(driver);
  assert(driver.values[0].alpha == 128 && driver.values[1].alpha == 0);
  driver.clear(); effect->seek(500); effect->render(driver);
  assert(driver.values[1].alpha == 128 && driver.values[2].alpha == 0);
  driver.clear(); effect->seek(999); effect->render(driver); assert(driver.values[3].alpha == 128);
  effect->seek(1000); assert(effect->is_finished());
  options.reverse = true; effect.reset(descriptor(options).create_instance());
  driver.clear(); effect->seek(250); effect->render(driver);
  assert(driver.values[3].alpha == 128 && driver.values[2].alpha == 0);
  options.loop = true; options.duration_ms = 0; effect.reset(descriptor(options).create_instance());
  driver.clear(); effect->seek(1250); effect->render(driver);
  assert(driver.values[3].alpha == 128 && driver.values[2].alpha == 0 && !effect->is_finished());
  options.fill_from_center = true; options.reverse = false;
  effect.reset(descriptor(options).create_instance());
  driver.clear(); effect->seek(250); effect->render(driver);
  assert(driver.values[0].alpha == 0 && driver.values[1].alpha == 128 &&
         driver.values[2].alpha == 128 && driver.values[3].alpha == 0);
  driver.clear(); effect->seek(999); effect->render(driver);
  for (const auto &value : driver.values) assert(value.alpha == 128);
  driver.count = 3;
  driver.clear(); effect->seek(250); effect->render(driver);
  assert(driver.values[0].alpha == 0 && driver.values[1].alpha == 128 && driver.values[2].alpha == 0);
  driver.clear(); effect->seek(999); effect->render(driver);
  for (size_t i = 0; i < 3; ++i) assert(driver.values[i].alpha == 128);
  driver.count = 4;
  driver.center = 1;
  driver.clear(); effect->seek(250); effect->render(driver);
  assert(driver.values[0].alpha == 128 && driver.values[1].alpha == 128 && driver.values[2].alpha == 0);
  driver.center = SIZE_MAX;
  linear_lighting_driver_view right(driver, 6, 2, 8);
  driver.clear(); effect->seek(250); effect->render(right);
  assert(driver.values[0].alpha == 0 && driver.values[1].alpha == 128 &&
         driver.values[2].alpha == 128 && driver.values[3].alpha == 0);
}
int main() {
  test_fill_effect();
  test_multi_color_gradient();
  test_keyframe_segments_and_ownership(); test_keyframe_interpolation_and_validation();
  test_blink_and_breathe(); test_independent_pixel_tracks(); test_flowing_gradient();
  test_keyframed_motion_width_and_wrapping();
  test_animated_layers_blend_once_per_frame();
  test_linear_canvas_views();
  test_rainbow_zero_offset_views();
  std::puts("Common animations: interpolation, timing, looping, blending, gradients, and validation passed.");
}
