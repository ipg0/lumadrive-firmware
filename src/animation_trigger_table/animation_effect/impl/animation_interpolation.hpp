#pragma once

#include "../animation_effect.hpp"

enum class animation_interpolation { LINEAR, STEP, SMOOTH };

namespace animation_math {
constexpr uint32_t SCALE = 65536;

inline uint32_t smooth(uint32_t fraction) {
  return (uint64_t(fraction) * fraction * (3 * SCALE - 2 * fraction) +
          uint64_t(SCALE) * SCALE / 2) / (uint64_t(SCALE) * SCALE);
}

inline uint32_t triangle(uint32_t phase) {
  return phase <= SCALE / 2 ? phase * 2 : (SCALE - phase) * 2;
}

inline uint8_t mix_channel(uint8_t from, uint8_t to, uint32_t fraction) {
  return (uint32_t(from) * (SCALE - fraction) + uint32_t(to) * fraction + SCALE / 2) / SCALE;
}

inline alpha_color mix(alpha_color from, alpha_color to, uint32_t fraction) {
  return alpha_color(color(mix_channel(from.base_color.r, to.base_color.r, fraction),
                           mix_channel(from.base_color.g, to.base_color.g, fraction),
                           mix_channel(from.base_color.b, to.base_color.b, fraction)),
                     mix_channel(from.alpha, to.alpha, fraction));
}
} // namespace animation_math

class timed_animation_effect_instance : public animation_effect_instance_base {
public:
  explicit timed_animation_effect_instance(uint32_t duration_ms) : _duration_ms(duration_ms) {}
  void start(uint32_t now_ms) override { _started_at_ms = now_ms; _elapsed_ms = 0; }
  void update(uint32_t now_ms) override { seek(static_cast<uint32_t>(now_ms - _started_at_ms)); }
  void seek(uint64_t elapsed_ms) override { _elapsed_ms = elapsed_ms; }
  bool is_finished() const override { return _duration_ms && _elapsed_ms >= _duration_ms; }
protected:
  uint64_t _elapsed_ms = 0;
private:
  uint32_t _duration_ms, _started_at_ms = 0;
};
