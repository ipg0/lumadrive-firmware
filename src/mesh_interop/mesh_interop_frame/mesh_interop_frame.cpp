#include "mesh_interop_frame.hpp"
#include "../mesh_interop.hpp"
#include "./impl/mesh_interop_frame_discover.hpp"
#include "./impl/mesh_interop_frame_discover_reply.hpp"
#include "./impl/mesh_interop_frame_subscribe.hpp"
#include "./impl/mesh_interop_frame_subscribe_reply.hpp"
#include "./impl/mesh_interop_frame_heartbeat.hpp"
#include "./impl/mesh_interop_frame_heartbeat_reply.hpp"
#include <cstring>
#include <esp_log.h>
#include <esp_now.h>

uint16_t mesh_interop_frame_base::_frame_id_seq = 0;

mesh_interop_frame_base::serialized_frame_t *
mesh_interop_frame_base::serialize_payload() const {
  auto *frame = reinterpret_cast<serialized_frame_t *>(
      new uint8_t[sizeof(serialized_frame_t)]);
  frame->frame_id = this->frame_id;
  frame->magic = mesh_interop::MESH_INTEROP_FRAME_MAGIC;
  frame->frame_type = static_cast<uint8_t>(this->type());
  frame->payload_size = 0;
  return frame;
}

mesh_interop_frame_base::frame_buffer_t *
mesh_interop_frame_base::serialize() const {
  serialized_frame_t *serialized_frame = this->serialize_payload();

  if (serialized_frame->payload_size > ESP_NOW_MAX_DATA_LEN - sizeof(serialized_frame_t)) {
    delete[] reinterpret_cast<uint8_t *>(serialized_frame);
    return nullptr;
  }
  uint8_t *buffer = new uint8_t[ESP_NOW_MAX_DATA_LEN];
  auto *frame = reinterpret_cast<serialized_frame_t *>(buffer);
  frame->frame_id = serialized_frame->frame_id;
  frame->magic = serialized_frame->magic;
  frame->frame_type = serialized_frame->frame_type;
  frame->payload_size = serialized_frame->payload_size;

  if (serialized_frame->payload_size > 0) {
    memcpy(frame->payload, serialized_frame->payload,
           serialized_frame->payload_size);
  }

  size_t total_size =
      sizeof(serialized_frame_t) + serialized_frame->payload_size;

  delete[] reinterpret_cast<uint8_t *>(serialized_frame);

  return new frame_buffer_t{buffer, total_size};
}

mesh_interop_frame_base *
mesh_interop_frame_base::instantiate_received_frame(
    mesh_interop_frame_type type, const uint16_t frame_id,
    const uint8_t mac[ESP_NOW_ETH_ALEN]) {
  switch (type) {
  case mesh_interop_frame_type::DISCOVER:
    return new mesh_interop_frame<mesh_interop_frame_type::DISCOVER>(
        mac, false, frame_id);
  case mesh_interop_frame_type::DISCOVER_REPLY:
    return new mesh_interop_frame<
        mesh_interop_frame_type::DISCOVER_REPLY>(mac, false, frame_id);
  case mesh_interop_frame_type::SUBSCRIBE:
    return new mesh_interop_frame<mesh_interop_frame_type::SUBSCRIBE>(
        mac, false, frame_id);
  case mesh_interop_frame_type::SUBSCRIBE_REPLY:
    return new mesh_interop_frame<
        mesh_interop_frame_type::SUBSCRIBE_REPLY>(mac, false, frame_id);
  case mesh_interop_frame_type::HEARTBEAT:
    return new mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT>(
        mac, false, frame_id);
  case mesh_interop_frame_type::HEARTBEAT_REPLY:
    return new mesh_interop_frame<
        mesh_interop_frame_type::HEARTBEAT_REPLY>(mac, false, frame_id);
  default:
    return nullptr;
  }
}

esp_err_t mesh_interop_frame_base::send() const {
  if (this->is_send == false) {
    ESP_LOGE("mesh_interop", "Cannot send a frame that was received");
    return ESP_ERR_INVALID_ARG;
  }
  ESP_LOGD("mesh_interop", "Sending frame with frame_id: %u",
           this->frame_id);
  mesh_interop_frame_base::frame_buffer_t *buffer = this->serialize();
  if (!buffer) return ESP_ERR_INVALID_SIZE;

  bool peer_created = mesh_interop::instance().add_peer_unless_exists(this->mac);

  esp_err_t err =
      esp_now_send(this->mac, (const uint8_t *)buffer->data, buffer->len);

  delete[] buffer->data;
  delete buffer;

  if (peer_created) {
    // clean up if peer added for one time send
    mesh_interop::instance().remove_peer(this->mac);
  }

  return err;
}

mesh_interop_frame_base *
mesh_interop_frame_base::parse(const uint8_t *data, size_t size,
                                  const uint8_t mac[ESP_NOW_ETH_ALEN]) {
  if (size < sizeof(serialized_frame_t) || size > ESP_NOW_MAX_DATA_LEN) {
    ESP_LOGV("mesh_interop",
             "Received data is too small to be a valid frame");
    return nullptr;
  }

  auto *frame =
      reinterpret_cast<serialized_frame_t *>(const_cast<uint8_t *>(data));

  if (frame->magic != mesh_interop::MESH_INTEROP_FRAME_MAGIC) {
    ESP_LOGV("mesh_interop", "Invalid magic number: 0x%08X, skipping frame",
             frame->magic);
    return nullptr;
  }

  mesh_interop_frame_base *frame_instance = instantiate_received_frame(
      static_cast<mesh_interop_frame_type>(frame->frame_type),
      frame->frame_id, mac);

  if (frame_instance == nullptr) {
    ESP_LOGV("mesh_interop", "Unknown frame type: 0x%02X",
             frame->frame_type);
    return nullptr;
  }

  if (frame->payload_size != size - sizeof(serialized_frame_t)) {
    ESP_LOGV("mesh_interop",
             "Invalid payload size: header=%zu payload=%zu packet=%zu",
             sizeof(serialized_frame_t), frame->payload_size, size);
    delete frame_instance;
    return nullptr;
  }

  frame_instance->apply_payload(frame->payload, frame->payload_size);

  return frame_instance;
};
