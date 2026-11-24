#pragma once

#include "../signal_source.hpp"
#include <cmath>

namespace signal_source {
enum class waveform : uint8_t { SINE, TRIANGLE, SQUARE, SAWTOOTH, REVERSE_SAWTOOTH };

struct virtual_config {
  waveform shape;
  uint32_t period_ms;
  float amplitude, offset, phase;
};

class virtual_source final : public source {
public:
  explicit virtual_source(const virtual_config& config) : _config(config) {}
  bool sample(uint64_t master_time_us, float& value) override {
    const uint64_t period = uint64_t(_config.period_ms) * 1000;
    if (!period)
      return false;
    float phase = float(master_time_us % period) / float(period) + _config.phase;
    phase -= std::floor(phase);
    float normalized;
    switch (_config.shape) {
    case waveform::SINE:
      normalized = (std::sin(phase * 6.28318530718f - 1.57079632679f) + 1) * 0.5f;
      break;
    case waveform::TRIANGLE:
      normalized = phase < 0.5f ? phase * 2 : (1 - phase) * 2;
      break;
    case waveform::SQUARE:
      normalized = phase < 0.5f ? 0 : 1;
      break;
    case waveform::SAWTOOTH:
      normalized = phase;
      break;
    case waveform::REVERSE_SAWTOOTH:
      normalized = 1 - phase;
      break;
    default:
      return false;
    }
    value = _config.offset + _config.amplitude * normalized;
    return std::isfinite(value);
  }

private:
  virtual_config _config;
};
} // namespace signal_source
