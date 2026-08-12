#pragma once

#include <driver/gpio.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <mutex>
#define CONFIG_DISCOVERY_GPIO GPIO_NUM_16

class esp_now_interop {

public:
  static constexpr uint32_t ESP_NOW_INTEROP_FRAME_MAGIC = 0xFAB17000;
  static constexpr uint32_t ESP_NOW_INTEROP_NVS_MAGIC =
      ESP_NOW_INTEROP_FRAME_MAGIC;
  static constexpr uint8_t ESP_NOW_CHANNEL = 6;
  static constexpr uint8_t ESP_NOW_INTEROP_SLAVE_NODES_MAX = 16;
  static constexpr uint32_t ESP_NOW_INTEROP_HEARTBEAT_INTERVAL_US = 1000000;
  static constexpr uint32_t ESP_NOW_INTEROP_MASTER_DISCOVERY_TIMEOUT_US =
      20000000;

  static esp_now_interop &instance() {
    static esp_now_interop instance;
    return instance;
  }

  esp_err_t start_discovery();
  esp_err_t finish_discovery();
  bool is_discovery_mode() {
    std::lock_guard<std::mutex> lock(this->state_mutex);
    return this->discovery_mode;
  }
  bool add_peer_unless_exists(const uint8_t[ESP_NOW_ETH_ALEN]);
  void remove_peer(const uint8_t[ESP_NOW_ETH_ALEN]);
  esp_err_t register_slave_node(const uint8_t[ESP_NOW_ETH_ALEN]);
  esp_err_t subscribe_to_master_node(const uint8_t[ESP_NOW_ETH_ALEN]);
  bool slave_node_exists(const uint8_t[ESP_NOW_ETH_ALEN]);
  bool master_node_matches(const uint8_t[ESP_NOW_ETH_ALEN]);
  esp_err_t sync_slave_node(const uint8_t[ESP_NOW_ETH_ALEN]);

private:
  typedef struct __attribute__((packed)) {
    uint8_t mac[ESP_NOW_ETH_ALEN];
  } esp_now_interop_node_t;

  typedef struct __attribute__((packed)) {
    uint32_t magic;
    bool is_configured;
    bool is_master_state;
    uint8_t nodes_count;
    esp_now_interop_node_t nodes[ESP_NOW_INTEROP_SLAVE_NODES_MAX];
  } esp_now_interop_state_t;

  std::mutex state_mutex;
  bool discovery_mode;
  esp_now_interop_state_t state;
  int64_t node_last_synced_at[ESP_NOW_INTEROP_SLAVE_NODES_MAX];

  void broadcast_discover();
  void send_heartbeats();
  bool is_interop_configured();
  esp_err_t load_state();
  esp_err_t save_state();
  esp_err_t clear_saved_state();
  void init_state();
  esp_err_t add_all_state_nodes_as_peers();
  size_t find_slave_node_index(const uint8_t mac[ESP_NOW_ETH_ALEN]);
  bool master_node_matches_impl(const uint8_t mac[ESP_NOW_ETH_ALEN]);
  esp_err_t start_discovery_impl();
  esp_err_t finish_discovery_impl();
  static void heartbeat_timer_cb(void *);
  static void finish_discovery_timer_cb(void *);
  static void handle_esp_now_recv(const esp_now_recv_info_t *, const uint8_t *,
                                  int);

  esp_now_interop();
  ~esp_now_interop() = default;
};
