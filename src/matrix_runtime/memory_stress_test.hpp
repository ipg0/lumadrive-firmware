#pragma once

// This test runs only in the opt-in memory_test environment. It does not write
// NVS or replace the active runtime. The caller runs it after radio startup.
#ifdef MATRIX_MEMORY_STRESS_TEST
#include "matrix_core.hpp"
#include "signal_buffer.hpp"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cstdlib>
#include <cstring>

namespace matrix {
inline void memory_test_report(const char* stage) {
  ESP_LOGI("memory_test", "%s: free=%u largest=%u minimum_free=%u", stage,
           unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)));
}

inline bool run_memory_stress_test(const runtime_config* active, const uint8_t* node_mac,
                                   driver_resolver resolver) {
  memory_test_report("baseline");
  if (!active || active->encoded().empty()) {
    ESP_LOGE("memory_test", "FAIL: no active encoded configuration to test");
    return false;
  }

  // Keep histories for two full revisions while staging the largest allowed
  // encoded record, as can happen during a configuration transition.
  signal_buffer history;
  for (uint64_t revision = 1; revision <= 2; ++revision) {
    for (uint32_t id = 1; id <= 32; ++id) {
      if (!history.update(revision, id, 1, 0.5f)) {
        ESP_LOGE("memory_test", "FAIL: signal history allocation, revision=%u id=%u",
                 unsigned(revision), unsigned(id));
        return false;
      }
    }
  }
  memory_test_report("64 signal histories");
  auto record = std::unique_ptr<uint8_t, decltype(&std::free)>(
      static_cast<uint8_t*>(std::malloc(MAX_BYTES + 24)), &std::free);
  if (!record) {
    ESP_LOGE("memory_test", "FAIL: maximum-sized saved record allocation");
    return false;
  }
  memory_test_report("saved record staged");
  auto decoded = make_heap_object<matrix_config>();
  bool restore_ok = false;
  if (!decoded) {
    ESP_LOGE("memory_test", "FAIL: decoded matrix allocation");
    memory_test_report("matrix allocation failed");
  } else {
    const auto& bytes = active->encoded();
    memcpy(record.get(), bytes.data(), bytes.size());
    restore_ok =
        decode(mesh_interop_MatrixConfig_fields, decoded.get(), record.get(), bytes.size());
    if (!restore_ok)
      ESP_LOGE("memory_test", "FAIL: matrix protobuf decode");
    else
      memory_test_report("matrix decoded");
  }
  decoded.reset();
  record.reset();

  // The actual active configuration remains live. Prepare replacement players
  // independently, without touching the service's pending or active players.
  auto ledger = make_heap_object<player>();
  auto local = make_heap_object<player>(resolver);
  if (!ledger || !local || !ledger->prepare(*active, nullptr, 1, 2) ||
      !local->prepare(*active, node_mac, 1, 2)) {
    ESP_LOGE("memory_test", "FAIL: replacement player preparation");
    memory_test_report("replacement failed");
    return false;
  }
  memory_test_report("replacement prepared");
  if (!restore_ok) {
    ESP_LOGE("memory_test", "FAIL: saved-record allocation; replacement prepared");
    return false;
  }
  ESP_LOGI("memory_test", "PASS: saved-record allocation and current-configuration replacement");
  return true;
}
} // namespace matrix
#endif
