#pragma once

#include "../mesh_interop_frame.hpp"

// forward declaration — full definition in mesh_interop_frame_discover.hpp
template <> class mesh_interop_frame<mesh_interop_frame_type::DISCOVER>;

template <>
class mesh_interop_frame<mesh_interop_frame_type::DISCOVER_REPLY>
    : public mesh_interop_frame_base {
  friend class mesh_interop_frame_base;

private:
  static constexpr const char *TAG = "mesh_interop";
  uint16_t _discover_frame_id;
  void apply_payload(const uint8_t *payload, size_t payload_size) override;

protected:
  mesh_interop_frame(const uint8_t mac[ESP_NOW_ETH_ALEN], bool is_send,
                     uint16_t id)
      : mesh_interop_frame_base(mac, is_send, id) {}
  serialized_frame_t *serialize_payload() const override;
  mesh_interop_frame(const uint8_t mac[ESP_NOW_ETH_ALEN])
      : mesh_interop_frame_base(mac) {}

public:
  explicit mesh_interop_frame(
      const mesh_interop_frame<mesh_interop_frame_type::DISCOVER>
          *discover_frame);
  mesh_interop_frame_type type() const override {
    return mesh_interop_frame_type::DISCOVER_REPLY;
  }
  esp_err_t handle() const override;
};
