#include <cstdint>
#include <esp_err.h>
#include <cstring>

enum class esp_now_interop_frame_type {
  DISCOVER = 0x01,
  DISCOVER_REPLY = 0x02
};

class esp_now_interop_frame_base {

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
  static uint16_t frame_id_seq;
  virtual void apply_payload(const uint8_t *, size_t) {
    // no meaningful payload by default
  };
  static esp_now_interop_frame_base *
  instantiate_received_frame(esp_now_interop_frame_type,
                             const uint16_t frame_id, const uint8_t mac[6]);
  frame_buffer_t *serialize() const;

protected:
  virtual serialized_frame_t *serialize_payload() const;
  esp_now_interop_frame_base(const uint8_t mac[6], bool is_send,
                             const uint16_t id)
      : frame_id(id), is_send(is_send) {
    memcpy(this->mac, mac, 6);
  };

public:
  uint8_t mac[6];
  const bool is_send;
  esp_now_interop_frame_base(const uint8_t mac[6])
      : esp_now_interop_frame_base(mac, true, frame_id_seq++) {};
  virtual ~esp_now_interop_frame_base() = default;
  virtual esp_err_t handle() const = 0;
  esp_err_t send() const;
  virtual esp_now_interop_frame_type type() const = 0;
  static esp_now_interop_frame_base *parse(const uint8_t *, size_t,
                                           const uint8_t mac[6]);
};

template <esp_now_interop_frame_type type> class esp_now_interop_frame;

template <>
class esp_now_interop_frame<esp_now_interop_frame_type::DISCOVER>
    : public esp_now_interop_frame_base {
  friend class esp_now_interop_frame_base;

protected:
  esp_now_interop_frame(const uint8_t mac[6], const bool is_send,
                        const uint16_t id)
      : esp_now_interop_frame_base(mac, is_send, id) {}
  esp_now_interop_frame(const uint8_t mac[6])
      : esp_now_interop_frame_base(mac) {};

public:
  esp_now_interop_frame()
      : esp_now_interop_frame(
            reinterpret_cast<const uint8_t *>("\xFF\xFF\xFF\xFF\xFF\xFF")) {};
  virtual esp_now_interop_frame_type type() const override {
    return esp_now_interop_frame_type::DISCOVER;
  }
  virtual esp_err_t handle() const override;
};

template <>
class esp_now_interop_frame<esp_now_interop_frame_type::DISCOVER_REPLY>
    : public esp_now_interop_frame_base {
  friend class esp_now_interop_frame_base;

private:
  uint16_t discover_frame_id;
  virtual void apply_payload(const uint8_t *payload,
                             size_t payload_size) override;

protected:
  esp_now_interop_frame(const uint8_t mac[6], const bool is_send,
                        const uint16_t id)
      : esp_now_interop_frame_base(mac, is_send, id) {}
  virtual serialized_frame_t *serialize_payload() const override;
  esp_now_interop_frame(const uint8_t mac[6])
      : esp_now_interop_frame_base(mac) {};

public:
  esp_now_interop_frame(
      const esp_now_interop_frame<esp_now_interop_frame_type::DISCOVER>
          *discover_frame);
  virtual esp_now_interop_frame_type type() const override {
    return esp_now_interop_frame_type::DISCOVER_REPLY;
  }
  virtual esp_err_t handle() const override;
};
