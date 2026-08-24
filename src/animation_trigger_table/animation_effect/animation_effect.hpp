#pragma once

#include <cstdint>

enum class animation_effect_type {
  STATIC_COLOR = 0,
};

class animation_effect_descriptor_base {
public:
  virtual ~animation_effect_descriptor_base() = default;
  virtual animation_effect_type type() const = 0;
  virtual animation_effect_instance_base *create_instance() const = 0;
};

class animation_effect_instance_base {
public:
  virtual ~animation_effect_instance_base() = default;
  virtual animation_effect_type type() const = 0;
  virtual void start(uint32_t now_ms) = 0;
  virtual void update(uint32_t now_ms) = 0;
  virtual bool is_finished() const = 0;
  virtual void render() const = 0;
};

template <animation_effect_type T> class animation_effect_descriptor;
template <animation_effect_type T> class animation_effect_instance;