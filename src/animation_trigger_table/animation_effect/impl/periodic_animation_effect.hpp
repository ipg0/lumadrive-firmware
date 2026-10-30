#pragma once

#include "animation_interpolation.hpp"
#include <new>
#include <array>
#include <algorithm>

constexpr size_t MAX_GRADIENT_COLORS = 8;

struct periodic_animation_options {
  alpha_color primary;
  alpha_color secondary;
  std::array<alpha_color, MAX_GRADIENT_COLORS> gradient_colors{};
  size_t gradient_color_count = 0; // Zero uses the original primary/secondary pair.
  uint32_t period_ms = 1000;
  uint32_t duration_ms = 0;
  uint32_t wavelength_pixels = 0;
  uint8_t duty_percent = 50;
  bool reverse = false;
  bool loop = false;
};

template <animation_effect_type effect_type>
class animation_effect_instance : public timed_animation_effect_instance {
  static_assert(effect_type == animation_effect_type::BLINK || effect_type == animation_effect_type::BREATHE ||
                effect_type == animation_effect_type::FLOWING_GRADIENT || effect_type == animation_effect_type::RAINBOW ||
                effect_type == animation_effect_type::FILL);
public:
  explicit animation_effect_instance(periodic_animation_options options)
      : timed_animation_effect_instance(options.duration_ms), _options(options) {}
  animation_effect_type type() const override { return effect_type; }
  esp_err_t render(rgb_lighting_driver &driver) const override {
    using namespace animation_math;
    if (_options.period_ms < 2 || !_options.duty_percent || _options.duty_percent > 100)
      return ESP_ERR_INVALID_ARG;
    if constexpr (effect_type == animation_effect_type::FLOWING_GRADIENT) {
      if (_options.gradient_color_count == 1 || _options.gradient_color_count > MAX_GRADIENT_COLORS) return ESP_ERR_INVALID_ARG;
    }
    const uint64_t time = _elapsed_ms % _options.period_ms;
    if constexpr (effect_type == animation_effect_type::BLINK) {
      auto value = _options.primary;
      if (time * 100 >= uint64_t(_options.period_ms) * _options.duty_percent) value.alpha = 0;
      return driver.fill(value);
    } else if constexpr (effect_type == animation_effect_type::BREATHE) {
      auto value = _options.primary;
      value.alpha = mix_channel(0, value.alpha, smooth(triangle(time * SCALE / _options.period_ms)));
      return driver.fill(value);
    } else if constexpr (effect_type == animation_effect_type::FILL) {
      const size_t count = driver.get_pixel_count();
      const size_t render_count = driver.get_render_pixel_count();
      if (count > size_t(UINT16_MAX) + 1 || render_count > size_t(UINT16_MAX) + 1) return ESP_ERR_INVALID_ARG;
      const uint64_t elapsed = _options.loop ? time : std::min<uint64_t>(_elapsed_ms, _options.period_ms);
      const size_t filled = (elapsed * count + _options.period_ms - 1) / _options.period_ms;
      for (size_t local = 0; local < render_count; ++local) {
        const auto pixel = driver.get_render_pixel_index(static_cast<uint16_t>(local));
        if (pixel >= count) return ESP_ERR_INVALID_ARG;
        if ((!_options.reverse && pixel < filled) || (_options.reverse && pixel >= count - filled)) {
          auto err = driver.set_pixel(pixel, _options.primary);
          if (err != ESP_OK) return err;
        }
      }
      return ESP_OK;
    } else {
      const size_t count = driver.get_pixel_count();
      const size_t render_count = driver.get_render_pixel_count();
      if (count > size_t(UINT16_MAX) + 1 || render_count > size_t(UINT16_MAX) + 1) return ESP_ERR_INVALID_ARG;
      const auto wavelength = _options.wavelength_pixels ? _options.wavelength_pixels : count;
      const auto phase = time * SCALE / _options.period_ms;
      for (size_t local = 0; local < render_count; ++local) {
        const auto pixel = driver.get_render_pixel_index(static_cast<uint16_t>(local));
        if (pixel >= count) return ESP_ERR_INVALID_ARG;
        const uint32_t position = uint64_t(pixel % wavelength) * SCALE / wavelength;
        const uint32_t shifted = (position + (_options.reverse ? phase : SCALE - phase)) % SCALE;
        alpha_color value;
        if constexpr (effect_type == animation_effect_type::RAINBOW) value = rainbow(shifted, _options.primary.alpha);
        else if (!_options.gradient_color_count) value = mix(_options.primary, _options.secondary, triangle(shifted));
        else {
          const uint64_t scaled = uint64_t(shifted) * _options.gradient_color_count;
          const auto stop = scaled / SCALE;
          value = mix(_options.gradient_colors[stop],
              _options.gradient_colors[(stop + 1) % _options.gradient_color_count], scaled % SCALE);
        }
        auto err = driver.set_pixel(static_cast<uint16_t>(pixel), value);
        if (err != ESP_OK) return err;
      }
      return ESP_OK;
    }
  }
private:
  periodic_animation_options _options;
};

template <animation_effect_type effect_type>
class animation_effect_descriptor : public animation_effect_descriptor_base {
public:
  explicit animation_effect_descriptor(periodic_animation_options options) : _options(options) {}
  animation_effect_type type() const override { return effect_type; }
  animation_effect_instance_base *create_instance() const override {
    if (_options.period_ms < 2 || !_options.duty_percent || _options.duty_percent > 100) return nullptr;
    if constexpr (effect_type == animation_effect_type::FLOWING_GRADIENT) {
      if (_options.gradient_color_count == 1 || _options.gradient_color_count > MAX_GRADIENT_COLORS) return nullptr;
    }
    return new (std::nothrow) animation_effect_instance<effect_type>(_options);
  }
private:
  periodic_animation_options _options;
};
