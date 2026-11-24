#pragma once
#include <cstddef>
#include <cstdint>
#include <esp_err.h>
#include <mesh.pb.h>

namespace matrix_service {
struct sensor_sample {
  uint32_t sensor_id;
  float value;
  uint64_t captured_local_us;
};
// Nonblocking input for future sensor drivers. Samples are batched by the worker.
bool submit_sensor_sample(const sensor_sample& sample);
esp_err_t init();
// Callback-safe: copies into bounded queues, never waits for transport/storage.
bool receive(const uint8_t mac[6], const uint8_t* data, size_t size);
bool upload(uint16_t connection, const uint8_t* data, size_t size);
void disconnect(uint16_t connection);
mesh_interop_MatrixStatus status();
size_t read_status(uint8_t* destination, size_t capacity);
size_t read_layout_response(uint8_t* destination, size_t capacity);
void set_status_callback(void (*callback)());
} // namespace matrix_service
