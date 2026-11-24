#pragma once

#include "animation_interpolation.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <new>

constexpr size_t MAX_ANIMATION_KEYFRAMES = 16;
struct animation_keyframe {
  uint32_t time_ms;
  alpha_color value;
  animation_interpolation interpolation = animation_interpolation::LINEAR;
  uint32_t track_id = 0;
  float position = 0;
  float width = 1;
  bool wrap = false;
  bool all_pixels = false;
};

inline bool same_keyframe_track(const animation_keyframe& a, const animation_keyframe& b) {
  return a.track_id == b.track_id;
}

inline bool valid_animation_keyframes(const animation_keyframe* frames, size_t count) {
  if (!frames || count < 2 || count > MAX_ANIMATION_KEYFRAMES)
    return false;
  for (size_t i = 0; i < count; ++i) {
    const auto& f = frames[i];
    if (!std::isfinite(f.position) || !std::isfinite(f.width) || std::abs(f.position) > 1000000 ||
        f.width < 0 || f.width > 65536 ||
        (f.interpolation != animation_interpolation::LINEAR &&
         frames[i].interpolation != animation_interpolation::STEP &&
         frames[i].interpolation != animation_interpolation::SMOOTH))
      return false;
    size_t track_count = 0;
    const animation_keyframe* previous = nullptr;
    for (size_t j = 0; j < count; ++j) {
      if (same_keyframe_track(f, frames[j])) {
        if (f.wrap != frames[j].wrap || f.all_pixels != frames[j].all_pixels)
          return false;
        ++track_count;
        if (j < i)
          previous = &frames[j];
      }
    }
    if (track_count < 2 || (previous ? f.time_ms <= previous->time_ms : f.time_ms != 0))
      return false;
  }
  return true;
}

template <>
class animation_effect_instance<animation_effect_type::KEYFRAMES>
    : public timed_animation_effect_instance {
public:
  animation_effect_instance(const animation_keyframe* frames, size_t count, bool loop,
                            uint32_t duration_ms)
      : timed_animation_effect_instance(duration_ms),
        _count(valid_animation_keyframes(frames, count) ? count : 0), _loop(loop) {
    for (size_t i = 0; i < _count; ++i) {
      _frames[i] = frames[i];
      if (frames[i].time_ms > _cycle_ms)
        _cycle_ms = frames[i].time_ms;
    }
  }
  animation_effect_type type() const override { return animation_effect_type::KEYFRAMES; }
  void seek(uint64_t elapsed_ms) override {
    _sampling = false;
    timed_animation_effect_instance::seek(elapsed_ms);
  }
  void seek_position(uint32_t position) override {
    _sampling = true;
    _elapsed_ms =
        uint64_t(_cycle_ms) * std::min(position, animation_math::SCALE) / animation_math::SCALE;
  }
  esp_err_t render(render_target& driver) const override {
    if (!_count)
      return ESP_ERR_INVALID_ARG;
    const auto time = _loop && !_sampling ? _elapsed_ms % _cycle_ms : _elapsed_ms;
    const auto pixel_count = driver.get_pixel_count();
    const auto render_count = driver.get_render_pixel_count();
    if (pixel_count > size_t(UINT16_MAX) + 1 || render_count > size_t(UINT16_MAX) + 1)
      return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < _count; ++i) {
      bool first = true;
      for (size_t j = 0; j < i; ++j)
        if (same_keyframe_track(_frames[i], _frames[j]))
          first = false;
      if (!first)
        continue;
      const auto* from = &_frames[i];
      const animation_keyframe* to = nullptr;
      for (size_t j = i + 1; j < _count; ++j) {
        if (!same_keyframe_track(*from, _frames[j]))
          continue;
        if (_frames[j].time_ms <= time)
          from = &_frames[j];
        else {
          to = &_frames[j];
          break;
        }
      }
      auto value = from->value;
      auto position = int64_t(from->position * animation_math::SCALE);
      auto width = int64_t(from->width * animation_math::SCALE);
      if (to) {
        uint32_t fraction =
            (time - from->time_ms) * animation_math::SCALE / (to->time_ms - from->time_ms);
        if (from->interpolation == animation_interpolation::STEP)
          fraction = 0;
        else if (from->interpolation == animation_interpolation::SMOOTH)
          fraction = animation_math::smooth(fraction);
        value = animation_math::mix(from->value, to->value, fraction);
        const auto next_position = int64_t(to->position * animation_math::SCALE);
        const auto next_width = int64_t(to->width * animation_math::SCALE);
        position += (next_position - position) * fraction / animation_math::SCALE;
        width += (next_width - width) * fraction / animation_math::SCALE;
      }
      if (from->all_pixels) {
        auto err = driver.fill(value);
        if (err != ESP_OK)
          return err;
      } else {
        const int64_t length = int64_t(pixel_count) * animation_math::SCALE;
        if (!length)
          continue;
        if (from->wrap)
          position = (position % length + length) % length;
        const auto end = position + width;
        for (size_t local = 0; local < render_count; ++local) {
          const auto pixel = driver.get_render_pixel_index(static_cast<uint16_t>(local));
          if (pixel >= pixel_count)
            return ESP_ERR_INVALID_ARG;
          const int64_t left = int64_t(pixel) * animation_math::SCALE,
                        right = left + animation_math::SCALE;
          auto coverage = std::max<int64_t>(0, std::min(right, end) - std::max(left, position));
          if (from->wrap) {
            if (width >= length)
              coverage = animation_math::SCALE;
            else if (end > length)
              coverage += std::max<int64_t>(0, std::min(right, end - length) - left);
          }
          if (!coverage)
            continue;
          auto covered = value;
          covered.alpha = animation_math::mix_channel(0, value.alpha, coverage);
          auto err = driver.set_pixel(static_cast<uint16_t>(pixel), covered);
          if (err != ESP_OK)
            return err;
        }
      }
    }
    return ESP_OK;
  }

private:
  std::array<animation_keyframe, MAX_ANIMATION_KEYFRAMES> _frames{};
  size_t _count;
  bool _loop;
  bool _sampling = false;
  uint32_t _cycle_ms = 0;
};

template <>
class animation_effect_descriptor<animation_effect_type::KEYFRAMES>
    : public animation_effect_descriptor_base {
public:
  // Instances copy the frames; descriptors do not borrow the caller's array.
  animation_effect_descriptor(const animation_keyframe* frames, size_t count, bool loop = false,
                              uint32_t duration_ms = 0)
      : _count(valid_animation_keyframes(frames, count) ? count : 0), _loop(loop),
        _duration_ms(duration_ms) {
    for (size_t i = 0; i < _count; ++i)
      _frames[i] = frames[i];
  }
  animation_effect_type type() const override { return animation_effect_type::KEYFRAMES; }
  animation_effect_instance_base* create_instance() const override {
    if (!_count)
      return nullptr;
    return new (std::nothrow) animation_effect_instance<animation_effect_type::KEYFRAMES>(
        _frames.data(), _count, _loop, _duration_ms);
  }

private:
  std::array<animation_keyframe, MAX_ANIMATION_KEYFRAMES> _frames{};
  size_t _count;
  bool _loop;
  uint32_t _duration_ms;
};
