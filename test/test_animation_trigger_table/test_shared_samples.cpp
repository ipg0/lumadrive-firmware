#include "animation_trigger_table/animation_effect/impl/periodic_animation_effect.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// This target deliberately has no hardware lifecycle or presentation methods.
class sample_target : public render_target {
public:
  explicit sample_target(size_t count) : pixels(count, alpha_color(color(0, 0, 0), 0)) {}
  size_t get_pixel_count() const override { return pixels.size(); }
  esp_err_t set_pixel(uint16_t index, alpha_color value) override {
    pixels.at(index) = value;
    return ESP_OK;
  }
  esp_err_t fill(alpha_color value) override {
    for (auto& pixel : pixels)
      pixel = value;
    return ESP_OK;
  }
  std::vector<alpha_color> pixels;
};

int main() {
  std::ifstream input("../proto/test_vectors/simple_animation.csv");
  assert(input.is_open());
  std::string line;
  unsigned samples = 0;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    char kind[24];
    unsigned elapsed, pixel, count, reverse, center, r, g, b, a;
    assert(std::sscanf(line.c_str(), "%23[^,],%u,%u,%u,%u,%u,%u,%u,%u,%u", kind, &elapsed, &pixel,
                       &count, &reverse, &center, &r, &g, &b, &a) == 10);
    periodic_animation_options options{};
    options.primary = alpha_color(color(255, 0, 0), 255);
    options.gradient_colors[0] = options.primary;
    options.gradient_colors[1] = alpha_color(color(0, 0, 255), 255);
    options.gradient_color_count = 2;
    options.period_ms = 1000;
    options.reverse = reverse;
    options.fill_from_center = center;
    std::unique_ptr<animation_effect_instance_base> effect;
    const std::string type(kind);
    if (type == "blink")
      effect.reset(new animation_effect_instance<animation_effect_type::BLINK>(options));
    else if (type == "breathe")
      effect.reset(new animation_effect_instance<animation_effect_type::BREATHE>(options));
    else if (type == "rainbow")
      effect.reset(new animation_effect_instance<animation_effect_type::RAINBOW>(options));
    else if (type == "fill")
      effect.reset(new animation_effect_instance<animation_effect_type::FILL>(options));
    else if (type == "gradient")
      effect.reset(new animation_effect_instance<animation_effect_type::FLOWING_GRADIENT>(options));
    assert(effect && pixel < count);
    sample_target target(count);
    effect->seek(elapsed);
    assert(effect->render(target) == ESP_OK);
    const auto result = target.pixels[pixel];
    assert(result.alpha == a);
    if (a) {
      assert(std::abs(int(result.base_color.r) - int(r)) <= 1);
      assert(std::abs(int(result.base_color.g) - int(g)) <= 1);
      assert(std::abs(int(result.base_color.b) - int(b)) <= 1);
    }
    ++samples;
  }
  assert(samples >= 10);
  std::puts("Shared animation samples passed on a hardware-independent render target.");
}
