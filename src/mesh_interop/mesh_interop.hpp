#pragma once

#include <driver/gpio.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <mutex>
#define CONFIG_DISCOVERY_GPIO GPIO_NUM_16

class mesh_interop {

public:
  static constexpr uint32_t MESH_INTEROP_FRAME_MAGIC = 0xFAB17000;
  static constexpr uint32_t MESH_INTEROP_NVS_MAGIC = MESH_INTEROP_FRAME_MAGIC;
  static constexpr uint8_t ESP_NOW_CHANNEL = 6;
  static constexpr uint8_t MESH_INTEROP_SLAVE_NODES_MAX = 16;
  static constexpr uint32_t MESH_INTEROP_HEARTBEAT_INTERVAL_US = 1000000;
  static constexpr uint32_t MESH_INTEROP_MASTER_DISCOVERY_TIMEOUT_US = 60000000;

  typedef struct {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    uint64_t last_synced_at;
  } mesh_interop_node_result_t;

  typedef struct {
    bool is_discovery_mode;
    uint8_t nodes_count;
    mesh_interop_node_result_t nodes[MESH_INTEROP_SLAVE_NODES_MAX];
  } mesh_interop_state_result_t;

  static mesh_interop &instance() {
    static mesh_interop instance;
    return instance;
  }

  esp_err_t start_discovery();
  esp_err_t finish_discovery();
  bool is_discovery_mode() {
    std::lock_guard<std::mutex> lock(this->_state_mutex);
    return this->_discovery_mode;
  }
  bool add_peer_unless_exists(const uint8_t[ESP_NOW_ETH_ALEN]);
  void remove_peer(const uint8_t[ESP_NOW_ETH_ALEN]);
  esp_err_t register_slave_node(const uint8_t[ESP_NOW_ETH_ALEN]);
  esp_err_t subscribe_to_master_node(const uint8_t[ESP_NOW_ETH_ALEN]);
  bool slave_node_exists(const uint8_t[ESP_NOW_ETH_ALEN]);
  bool master_node_matches(const uint8_t[ESP_NOW_ETH_ALEN]);
  esp_err_t sync_slave_node(const uint8_t[ESP_NOW_ETH_ALEN]);
  mesh_interop_state_result_t get_state();
  void register_mesh_state_cb(void (*cb)());

private:
  typedef struct __attribute__((packed)) {
    uint8_t mac[ESP_NOW_ETH_ALEN];
  } mesh_interop_node_t;

  typedef struct __attribute__((packed)) {
    uint32_t magic;
    bool is_configured;
    bool is_master_state;
    uint8_t nodes_count;
    mesh_interop_node_t nodes[MESH_INTEROP_SLAVE_NODES_MAX];
  } mesh_interop_state_t;

  static constexpr const char *TAG = "mesh_interop";
  std::mutex _state_mutex;
  bool _discovery_mode;
  mesh_interop_state_t _state;
  int64_t _node_last_synced_at[MESH_INTEROP_SLAVE_NODES_MAX];
  void (*_mesh_state_cb)();

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
  void notify_mesh_state_cb();

  mesh_interop();
  ~mesh_interop() = default;
};
