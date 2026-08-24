#pragma once

#include <cstdint>

class color {
public:
  color() : r(0), g(0), b(0) {}
  color(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) {}
  uint8_t r;
  uint8_t g;
  uint8_t b;

  color rotate_hue(float degrees) const;
};

// a color that can be stacked on top of another color
class alpha_color {
public:
  alpha_color() : base_color(), alpha(0) {}
  alpha_color(color base_color) : base_color(base_color), alpha(255) {}
  alpha_color(color base_color, uint8_t a) : base_color(base_color), alpha(a) {}

  color base_color;
  uint8_t alpha;

  color blend_with(const color &other) const;
};