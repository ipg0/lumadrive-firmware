#pragma once

#include "../mesh_interop_frame.hpp"

// forward declaration — full definition in mesh_interop_frame_subscribe.hpp
template <> class mesh_interop_frame<mesh_interop_frame_type::SUBSCRIBE>;

template <>
class mesh_interop_frame<mesh_interop_frame_type::SUBSCRIBE_REPLY>
    : public mesh_interop_frame_base {
  friend class mesh_interop_frame_base;

public:
  typedef struct __attribute__((packed)) {
    uint16_t subscribe_frame_id;
    mesh_interop_subscribe_status subscribe_status;
  } subscribe_reply_payload_t;

private:
  static constexpr const char *TAG = "mesh_interop";
  uint16_t _subscribe_frame_id;
  mesh_interop_subscribe_status _subscribe_status;
  void apply_payload(const uint8_t *payload, size_t payload_size) override;

protected:
  mesh_interop_frame(const uint8_t mac[ESP_NOW_ETH_ALEN], bool is_send,
                     uint16_t id)
      : mesh_interop_frame_base(mac, is_send, id) {}
  serialized_frame_t *serialize_payload() const override;
  mesh_interop_frame(const uint8_t mac[ESP_NOW_ETH_ALEN])
      : mesh_interop_frame_base(mac) {}

public:
  mesh_interop_frame(
      const mesh_interop_frame<mesh_interop_frame_type::SUBSCRIBE>
          *subscribe_frame,
      mesh_interop_subscribe_status status);
  mesh_interop_frame_type type() const override {
    return mesh_interop_frame_type::SUBSCRIBE_REPLY;
  }
  esp_err_t handle() const override;
};
