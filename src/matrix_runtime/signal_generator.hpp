#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mesh.pb.h>

namespace matrix {
constexpr uint32_t SIGNAL_SCALE = 65536;

inline bool valid_signal(const mesh_interop_SignalGenerator& signal) {
  if (!signal.id || !signal.name[0] || !std::isfinite(signal.amplitude) ||
      !std::isfinite(signal.offset) || !std::isfinite(signal.phase))
    return false;
  if (signal.kind == mesh_interop_SignalGenerator_Kind_SENSOR)
    return signal.sensor_id && !(signal.node_mac[0] & 1) &&
           std::any_of(signal.node_mac, signal.node_mac + 6, [](uint8_t b) { return b != 0; });
  return signal.kind >= mesh_interop_SignalGenerator_Kind_SINE &&
         signal.kind <= mesh_interop_SignalGenerator_Kind_REVERSE_SAWTOOTH &&
         signal.period_ms >= 20;
}

inline bool valid_modulation(const mesh_interop_AnimationModulation& modulation) {
  return modulation.signal_id && std::isfinite(modulation.minimum) &&
         std::isfinite(modulation.maximum) && modulation.minimum < modulation.maximum &&
         modulation.curve >= mesh_interop_AnimationModulation_Curve_LINEAR &&
         modulation.curve <= mesh_interop_AnimationModulation_Curve_SMOOTH;
}

// The generator is anchored to the shared master session clock, not to entry start.
inline float virtual_signal_value(const mesh_interop_SignalGenerator& signal, uint64_t clock_us) {
  const uint64_t period = uint64_t(signal.period_ms) * 1000;
  if (!period)
    return signal.offset;
  float phase = float(clock_us % period) / float(period) + signal.phase;
  phase -= std::floor(phase);
  float value = 0;
  switch (signal.kind) {
  case mesh_interop_SignalGenerator_Kind_SINE:
    value = (std::sin(phase * 6.28318530718f - 1.57079632679f) + 1) * 0.5f;
    break;
  case mesh_interop_SignalGenerator_Kind_TRIANGLE:
    value = phase < 0.5f ? phase * 2 : (1 - phase) * 2;
    break;
  case mesh_interop_SignalGenerator_Kind_SQUARE:
    value = phase < 0.5f ? 0 : 1;
    break;
  case mesh_interop_SignalGenerator_Kind_SAWTOOTH:
    value = phase;
    break;
  case mesh_interop_SignalGenerator_Kind_REVERSE_SAWTOOTH:
    value = 1 - phase;
    break;
  default:
    return signal.offset;
  }
  return signal.offset + signal.amplitude * value;
}

inline uint32_t map_signal(float value, const mesh_interop_AnimationModulation& modulation) {
  float x = std::clamp((value - modulation.minimum) / (modulation.maximum - modulation.minimum),
                       0.0f, 1.0f);
  switch (modulation.curve) {
  case mesh_interop_AnimationModulation_Curve_EASE_IN:
    x *= x;
    break;
  case mesh_interop_AnimationModulation_Curve_EASE_OUT:
    x = 1 - (1 - x) * (1 - x);
    break;
  case mesh_interop_AnimationModulation_Curve_SMOOTH:
    x = x * x * (3 - 2 * x);
    break;
  default:
    break;
  }
  return static_cast<uint32_t>(x * SIGNAL_SCALE + 0.5f);
}
} // namespace matrix
