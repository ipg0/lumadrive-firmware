#pragma once

#include "../animation_effect.hpp"
#include <new>

template <>
class animation_effect_instance<animation_effect_type::STATIC_COLOR>
    : public animation_effect_instance_base {
public:
  animation_effect_instance(alpha_color color, uint32_t duration_ms)
      : _color(color), _duration_ms(duration_ms) {}

  animation_effect_type type() const override {
    return animation_effect_type::STATIC_COLOR;
  }

  void start(uint32_t now_ms) override {
    _started_at_ms = now_ms;
    _finished = false;
  }

  void update(uint32_t now_ms) override {
    seek(static_cast<uint32_t>(now_ms - _started_at_ms));
  }

  void seek(uint64_t elapsed_ms) override { _finished = _duration_ms && elapsed_ms >= _duration_ms; }

  bool is_finished() const override { return _finished; }

  esp_err_t render(rgb_lighting_driver &driver) const override {
    return driver.fill(_color);
  }

private:
  alpha_color _color;
  uint32_t _duration_ms;
  uint32_t _started_at_ms = 0;
  bool _finished = false;
};

template <>
class animation_effect_descriptor<animation_effect_type::STATIC_COLOR>
    : public animation_effect_descriptor_base {
public:
  // A zero duration holds the color until the entry is stopped or removed.
  explicit animation_effect_descriptor(alpha_color color,
                                       uint32_t duration_ms = 0)
      : _color(color), _duration_ms(duration_ms) {}

  animation_effect_type type() const override {
    return animation_effect_type::STATIC_COLOR;
  }

  animation_effect_instance_base *create_instance() const override {
    return new (std::nothrow)
        animation_effect_instance<animation_effect_type::STATIC_COLOR>(
            _color, _duration_ms);
  }

private:
  alpha_color _color;
  uint32_t _duration_ms;
};
