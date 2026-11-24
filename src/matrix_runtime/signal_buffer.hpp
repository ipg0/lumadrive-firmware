#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <new>

namespace matrix {
// Two revisions can coexist while a matrix replacement is pending.
class signal_buffer {
public:
  static constexpr uint64_t STALE_US = 250000;
  void clear() {
    for (auto& entry : _slots)
      entry.reset();
  }
  bool update(uint64_t revision, uint32_t id, uint64_t time, float value) {
    if (!revision || !id || !std::isfinite(value))
      return false;
    slot* target = nullptr;
    for (auto& entry : _slots)
      if (entry && entry->id == id && entry->revision == revision) {
        target = entry.get();
        break;
      }
    if (!target)
      for (auto& entry : _slots)
        if (!entry) {
          entry.reset(new (std::nothrow) slot{});
          target = entry.get();
          break;
        }
    if (!target)
      return false;
    if (!target->id) {
      target->id = id;
      target->revision = revision;
    }
    if (target->samples[3].time && time <= target->samples[3].time)
      return true;
    for (size_t i = 0; i < 3; ++i)
      target->samples[i] = target->samples[i + 1];
    target->samples[3] = {time, value};
    return true;
  }
  bool lookup(uint64_t revision, uint32_t id, uint64_t time, float& value) const {
    for (const auto& pointer : _slots) {
      if (!pointer)
        continue;
      const auto& entry = *pointer;
      if (entry.id != id || entry.revision != revision || !entry.samples[3].time)
        continue;
      if (time >= entry.samples[3].time) {
        if (time - entry.samples[3].time > STALE_US)
          return false;
        value = entry.samples[3].value;
        return true;
      }
      for (size_t i = 0; i < 3; ++i) {
        const auto &before = entry.samples[i], &after = entry.samples[i + 1];
        if (!before.time || time < before.time || time > after.time)
          continue;
        const float fraction = float(time - before.time) / float(after.time - before.time);
        value = before.value + (after.value - before.value) * fraction;
        return true;
      }
      return false;
    }
    return false;
  }
  void retain(uint64_t first, uint64_t second = 0) {
    for (auto& entry : _slots)
      if (entry && entry->revision != first && entry->revision != second)
        entry.reset();
  }

private:
  struct sample {
    uint64_t time = 0;
    float value = 0;
  };
  struct slot {
    uint64_t revision = 0;
    uint32_t id = 0;
    std::array<sample, 4> samples{};
  };
  std::array<std::unique_ptr<slot>, 64> _slots{};
};
} // namespace matrix
