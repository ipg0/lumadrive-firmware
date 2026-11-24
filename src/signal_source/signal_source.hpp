#pragma once

#include <atomic>
#include <cstdint>

namespace signal_source {

// Sources are sampled on the synchronized master clock in worker context.
class source {
public:
  virtual ~source() = default;
  virtual bool init() { return true; }
  virtual void deinit() {}
  virtual bool sample(uint64_t master_time_us, float& value) = 0;
};

// Hardware drivers submit timestamped samples without depending on matrix code.
struct sample {
  uint32_t sensor_id;
  float value;
  uint64_t captured_local_us;
};
using sample_sink = bool (*)(const sample&);
inline std::atomic<sample_sink> registered_sink{nullptr};
inline void set_sample_sink(sample_sink sink) { registered_sink.store(sink); }
inline bool submit_sample(const sample& value) {
  const auto sink = registered_sink.load();
  return sink && sink(value);
}
inline bool publish_sample(source& input, uint32_t id, uint64_t master_time_us,
                           uint64_t captured_local_us) {
  float value;
  return input.sample(master_time_us, value) && submit_sample({id, value, captured_local_us});
}
} // namespace signal_source
