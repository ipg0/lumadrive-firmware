#pragma once

#include "../mesh_interop_frame.hpp"

// forward declaration — full definition in mesh_interop_frame_heartbeat.hpp
template <> class mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT>;

template <>
class mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT_REPLY>
    : public mesh_interop_frame_base {
  friend class mesh_interop_frame_base;

private:
  static constexpr const char *TAG = "mesh_interop";
  int64_t _heartbeat_frame_id;
  void apply_payload(const uint8_t *payload, size_t payload_size) override;

protected:
  mesh_interop_frame(const uint8_t mac[ESP_NOW_ETH_ALEN], bool is_send,
                     uint16_t id)
      : mesh_interop_frame_base(mac, is_send, id) {}
  serialized_frame_t *serialize_payload() const override;
  mesh_interop_frame(const uint8_t mac[ESP_NOW_ETH_ALEN]);

public:
  explicit mesh_interop_frame(
      const mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT>
          *heartbeat_frame);
  mesh_interop_frame_type type() const override {
    return mesh_interop_frame_type::HEARTBEAT_REPLY;
  }
  esp_err_t handle() const override;
};
