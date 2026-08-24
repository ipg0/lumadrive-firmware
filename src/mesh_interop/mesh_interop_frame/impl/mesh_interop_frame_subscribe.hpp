#pragma once

#include "../mesh_interop_frame.hpp"

template <>
class mesh_interop_frame<mesh_interop_frame_type::SUBSCRIBE>
    : public mesh_interop_frame_base {
  friend class mesh_interop_frame_base;

private:
  static constexpr const char *TAG = "mesh_interop";

protected:
  mesh_interop_frame(const uint8_t mac[ESP_NOW_ETH_ALEN], bool is_send,
                     uint16_t id)
      : mesh_interop_frame_base(mac, is_send, id) {}

public:
  explicit mesh_interop_frame(const uint8_t mac[ESP_NOW_ETH_ALEN])
      : mesh_interop_frame_base(mac) {}
  mesh_interop_frame_type type() const override {
    return mesh_interop_frame_type::SUBSCRIBE;
  }
  esp_err_t handle() const override;
};
