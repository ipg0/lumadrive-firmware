#pragma once

#include <cstdint>
#include <cstring>
#include <esp_err.h>
#include <esp_now.h>

enum class mesh_interop_frame_type {
  DISCOVER = 0x01,
  DISCOVER_REPLY = 0x02,
  SUBSCRIBE = 0x03,
  SUBSCRIBE_REPLY = 0x04,
  HEARTBEAT = 0x05,
  HEARTBEAT_REPLY = 0x06,
};

enum class mesh_interop_subscribe_status {
  OK = 0x00,
  REJECT = 0x01,
};

class mesh_interop_frame_base {

public:
  typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t frame_id;
    uint8_t frame_type;
    size_t payload_size;
    uint8_t payload[];
  } serialized_frame_t;

  typedef struct {
    uint8_t *data;
    size_t len;
  } frame_buffer_t;

  const uint16_t frame_id;

private:
  static uint16_t _frame_id_seq;
  virtual void apply_payload(const uint8_t *, size_t) {
    // no meaningful payload by default
  };
  static mesh_interop_frame_base *
  instantiate_received_frame(mesh_interop_frame_type,
                             const uint16_t frame_id,
                             const uint8_t mac[ESP_NOW_ETH_ALEN]);
  frame_buffer_t *serialize() const;

protected:
  virtual serialized_frame_t *serialize_payload() const;
  mesh_interop_frame_base(const uint8_t mac[ESP_NOW_ETH_ALEN], bool is_send,
                             const uint16_t id)
      : frame_id(id), is_send(is_send) {
    memcpy(this->mac, mac, ESP_NOW_ETH_ALEN);
  };

public:
  uint8_t mac[ESP_NOW_ETH_ALEN];
  const bool is_send;
  mesh_interop_frame_base(const uint8_t mac[ESP_NOW_ETH_ALEN])
      : mesh_interop_frame_base(mac, true, _frame_id_seq++) {};
  virtual ~mesh_interop_frame_base() = default;
  virtual esp_err_t handle() const = 0;
  esp_err_t send() const;
  virtual mesh_interop_frame_type type() const = 0;
  static mesh_interop_frame_base *parse(const uint8_t *, size_t,
                                           const uint8_t mac[ESP_NOW_ETH_ALEN]);
};

template <mesh_interop_frame_type type> class mesh_interop_frame;
