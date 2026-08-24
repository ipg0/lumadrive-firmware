#pragma once

#include "../mesh_interop_frame.hpp"

template <>
class mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT>
    : public mesh_interop_frame_base {
  friend class mesh_interop_frame_base;

private:
  static constexpr const char *TAG = "mesh_interop";
  int64_t _master_timestamp;
  void apply_payload(const uint8_t *payload, size_t payload_size) override;

protected:
  mesh_interop_frame(const uint8_t mac[ESP_NOW_ETH_ALEN], bool is_send,
                     uint16_t id)
      : mesh_interop_frame_base(mac, is_send, id) {}
  serialized_frame_t *serialize_payload() const override;

public:
  explicit mesh_interop_frame(const uint8_t mac[ESP_NOW_ETH_ALEN]);
  mesh_interop_frame_type type() const override {
    return mesh_interop_frame_type::HEARTBEAT;
  }
  esp_err_t handle() const override;
};
