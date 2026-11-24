#pragma once

#include "../../lighting_driver/render_target.hpp"
#include <cstdint>

class animation_effect_instance_base;

enum class animation_effect_type {
  STATIC_COLOR = 0,
  KEYFRAMES,
  BLINK,
  BREATHE,
  FLOWING_GRADIENT,
  RAINBOW,
  FILL,
};

class animation_effect_descriptor_base {
public:
  virtual ~animation_effect_descriptor_base() = default;
  virtual animation_effect_type type() const = 0;
  // Return a new instance owned by the caller, or nullptr on allocation failure.
  virtual animation_effect_instance_base* create_instance() const = 0;
};

class animation_effect_instance_base {
public:
  virtual ~animation_effect_instance_base() = default;
  virtual animation_effect_type type() const = 0;
  virtual void start(uint32_t now_ms) = 0;
  virtual void update(uint32_t now_ms) = 0;
  // Distributed playback seeks directly to a phase, including after long uptime.
  virtual void seek(uint64_t elapsed_ms) {
    update(elapsed_ms > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(elapsed_ms));
  }
  virtual void seek_position(uint32_t position) {
    (void)position;
    seek(0);
  }
  virtual bool is_finished() const = 0;
  // Paint a canvas; clearing and presenting are the caller's responsibility.
  virtual esp_err_t render(render_target& driver) const = 0;
};

template <animation_effect_type effect_type> class animation_effect_descriptor;
template <animation_effect_type effect_type> class animation_effect_instance;
