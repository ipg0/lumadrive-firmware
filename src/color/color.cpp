#include <cmath>
#include "color.hpp"

color alpha_color::blend_with(const color &other) const {
  if (alpha == 255) {
    return base_color;
  }
  if (alpha == 0) {
    return other;
  }

  uint16_t r = (base_color.r * alpha + other.r * (255 - alpha)) / 255;
  uint16_t g = (base_color.g * alpha + other.g * (255 - alpha)) / 255;
  uint16_t b = (base_color.b * alpha + other.b * (255 - alpha)) / 255;
  return color(r, g, b);
}

color color::rotate_hue(float degrees) const {
  float h = 0, s = 0, v = 0;
  float r = static_cast<float>(this->r) / 255.0f;
  float g = static_cast<float>(this->g) / 255.0f;
  float b = static_cast<float>(this->b) / 255.0f;

  float max = std::max(std::max(r, g), b);
  float min = std::min(std::min(r, g), b);
  v = max;

  float delta = max - min;
  if (delta < 0.00001f) {
    s = 0;
    h = 0;        // undefined
    return *this; // return original color if no saturation
  }
  if (max > 0.0f) {
    s = delta / max;
  } else {
    s = 0.0f;
    h = NAN;      // undefined
    return *this; // return original color if no saturation
  }

  if (r >= max)
    h = (g - b) / delta; // between yellow & magenta
  else if (g >= max)
    h = 2.0f + (b - r) / delta; // between cyan & yellow
  else
    h = 4.0f + (r - g) / delta; // between magenta & cyan

  h *= 60.0f; // convert to degrees
  if (h < 0.0f)
    h += 360.0f;

  // Rotate hue
  h += degrees;
  while (h >= 360.0f)
    h -= 360.0f;
  while (h < 0.0f)
    h += 360.0f;
  // Convert back to RGB
  int i = static_cast<int>(h / 60.0f) % 6;
  float f = (h / 60.0f) - i;
  float p = v * (1.0f - s);
  float q = v * (1.0f - f * s);
  float t = v * (1.0f - (1.0f - f) * s);
  float rr, gg, bb;
  switch (i) {
  case 0:
    rr = v;
    gg = t;
    bb = p;
    break;
  case 1:
    rr = q;
    gg = v;
    bb = p;
    break;
  case 2:
    rr = p;
    gg = v;
    bb = t;
    break;
  case 3:
    rr = p;
    gg = q;
    bb = v;
    break;
  case 4:
    rr = t;
    gg = p;
    bb = v;
    break;
  case 5:
  default:
    rr = v;
    gg = p;
    bb = q;
    break;
  }

  return color(static_cast<uint8_t>(rr * 255.0f),
               static_cast<uint8_t>(gg * 255.0f),
               static_cast<uint8_t>(bb * 255.0f));
}