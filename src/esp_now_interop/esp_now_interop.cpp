#include "esp_now_interop.hpp"
#include "../global_config/global_config.hpp"
#include "esp_now_interop_frame/esp_now_interop_frame.hpp"
#include <esp_log.h>
#include <esp_wifi.h>

void esp_now_interop::handle_esp_now_recv(
    const esp_now_recv_info_t *esp_now_info, const uint8_t *data,
    int data_len) {
  auto *frame =
      esp_now_interop_frame_base::parse(data, data_len, esp_now_info->src_addr);
  if (frame == nullptr) {
    ESP_LOGE("esp_now_interop", "Failed to parse received frame");
    return;
  }

  ESP_LOGI("esp_now_interop", "Received frame with frame_id: %u",
           frame->frame_id);

  frame->handle();

  delete frame;
}

void esp_now_interop::heartbeat_timer_cb(void *arg) {
  if (!global_config::instance().is_master_node())
    return; // timer shouldn't exist on non-master node

  esp_now_interop &interop = esp_now_interop::instance();

  if (interop.is_discovery_mode()) {
    interop.broadcast_discover();
  } else {
    interop.broadcast_heartbeat();
  }
}

esp_now_interop::esp_now_interop() {
  const wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_ERROR_CHECK(esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE));

  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(
      esp_now_register_recv_cb(&esp_now_interop::handle_esp_now_recv));

  // add broadcast MAC as peer by default
  esp_now_peer_info_t broadcast_peer_info = {};
  memset(&broadcast_peer_info, 0, sizeof(broadcast_peer_info));
  broadcast_peer_info.channel = 6;
  broadcast_peer_info.encrypt = false;
  memcpy(broadcast_peer_info.peer_addr, "\xFF\xFF\xFF\xFF\xFF\xFF",
         ESP_NOW_ETH_ALEN);

  ESP_ERROR_CHECK(esp_now_add_peer(&broadcast_peer_info));

  // defaults
  this->state.nodes_count = 0;
  this->state.is_configured = false;
  this->state.is_master_state = global_config::instance().is_master_node();

  // TODO: get saved state from NVS, check if current master/slave state matches
  // saved state, if not, reset

  if (!this->is_interop_configured()) {
    this->start_discovery_impl();
  }

  if (global_config::instance().is_master_node()) {
    // start heartbeat timer
    const esp_timer_create_args_t heartbeat_timer_args = {
        .callback = &esp_now_interop::heartbeat_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "heartbeat_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t heartbeat_timer;
    ESP_ERROR_CHECK(esp_timer_create(&heartbeat_timer_args, &heartbeat_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(heartbeat_timer, 1000000));
  }
}

void esp_now_interop::finish_discovery_timer_cb(void *arg) {
  esp_now_interop::instance().finish_discovery_impl();
}

esp_err_t esp_now_interop::start_discovery_impl() {
  this->discovery_mode = true;

  // TODO: clear saved peers

  if (global_config::instance().is_master_node()) {
    this->state.is_configured = false;
    this->state.nodes_count = 0;

    const esp_timer_create_args_t finish_discovery_timer_args = {
        .callback = &esp_now_interop::finish_discovery_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "finish_discovery_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t finish_discovery_timer;
    ESP_ERROR_CHECK(esp_timer_create(&finish_discovery_timer_args,
                                     &finish_discovery_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(finish_discovery_timer, 10000000));
  } else {
    this->state.is_configured = false;
  }

  return ESP_OK;
}

esp_err_t esp_now_interop::start_discovery() {
  std::lock_guard<std::mutex> lock(this->state_mutex);
  return this->start_discovery_impl();
}

esp_err_t esp_now_interop::finish_discovery_impl() {
  this->discovery_mode = false;

  if (global_config::instance().is_master_node()) {
    ESP_LOGI("esp_now_interop",
             "Discovery finished, %u slave nodes registered:",
             this->state.nodes_count);

    for (size_t i = 0; i < this->state.nodes_count; i++) {
      const auto &slave_node = this->state.nodes[i];
      ESP_LOGI("esp_now_interop",
               "Slave node %u: %02X:%02X:%02X:%02X:%02X:%02X", i,
               slave_node.mac[0], slave_node.mac[1], slave_node.mac[2],
               slave_node.mac[3], slave_node.mac[4], slave_node.mac[5]);
    }
  } else {
    ESP_LOGI("esp_now_interop",
             "Discovery finished, master node: %02X:%02X:%02X:%02X:%02X:%02X",
             this->state.nodes[0].mac[0], this->state.nodes[0].mac[1],
             this->state.nodes[0].mac[2], this->state.nodes[0].mac[3],
             this->state.nodes[0].mac[4], this->state.nodes[0].mac[5]);
  }

  // TODO: save discovered peers/master to NVS

  return ESP_OK;
}

esp_err_t esp_now_interop::finish_discovery() {
  std::lock_guard<std::mutex> lock(this->state_mutex);
  return this->finish_discovery_impl();
}

void esp_now_interop::broadcast_discover() {
  esp_now_interop_frame<esp_now_interop_frame_type::DISCOVER> discover_frame;
  esp_err_t send_err = discover_frame.send();
  if (send_err != ESP_OK) {
    ESP_LOGE("esp_now_interop", "Failed to send discover frame: %s",
             esp_err_to_name(send_err));
  }
}

void esp_now_interop::broadcast_heartbeat() {
  // broadcast heartbeat frame to all peers
  if (!global_config::instance().is_master_node())
    return; // only master node should send heartbeat

  for (size_t i = 0; i < this->state.nodes_count; i++) {
    const auto &slave_node = this->state.nodes[i];

    // TODO: send heartbeat frame to slave node
  }
}

bool esp_now_interop::is_interop_configured() {
  std::lock_guard<std::mutex> lock(this->state_mutex);

  if (global_config::instance().is_master_node()) {
    return this->state.is_configured && this->state.nodes_count > 0;
  } else {
    return this->state.is_configured;
  }
}

bool esp_now_interop::add_peer_unless_exists(
    const uint8_t mac[ESP_NOW_ETH_ALEN]) {
  if (esp_now_is_peer_exist(mac)) {
    return false; // peer already exists
  }

  esp_now_peer_info_t peer_info = {};
  memset(&peer_info, 0, sizeof(peer_info));
  peer_info.channel = ESP_NOW_CHANNEL;
  peer_info.encrypt = false;
  memcpy(peer_info.peer_addr, mac, ESP_NOW_ETH_ALEN);

  esp_err_t add_peer_err = esp_now_add_peer(&peer_info);

  if (add_peer_err == ESP_ERR_ESPNOW_EXIST) {
    return false;
  }

  if (add_peer_err != ESP_OK) {
    ESP_LOGE("esp_now_interop", "Failed to add peer: %s",
             esp_err_to_name(add_peer_err));
    return false;
  }

  return true;
}

void esp_now_interop::remove_peer(const uint8_t mac[ESP_NOW_ETH_ALEN]) {
  esp_err_t remove_peer_err = esp_now_del_peer(mac);
  if (remove_peer_err != ESP_OK &&
      remove_peer_err != ESP_ERR_ESPNOW_NOT_FOUND) {
    ESP_LOGE("esp_now_interop", "Failed to remove peer: %s",
             esp_err_to_name(remove_peer_err));
  }
}

esp_err_t
esp_now_interop::subscribe_to_master_node(const uint8_t mac[ESP_NOW_ETH_ALEN]) {
  std::lock_guard<std::mutex> lock(this->state_mutex);
  if (global_config::instance().is_master_node()) {
    ESP_LOGE("esp_now_interop",
             "Cannot subscribe to master node on a master node");
    return ESP_ERR_INVALID_ARG;
  }

  this->state.is_configured = true;
  this->state.nodes_count = 1;
  memcpy(this->state.nodes[0].mac, mac, ESP_NOW_ETH_ALEN);

  // add master as permanent peer
  add_peer_unless_exists(mac);

  return this->finish_discovery_impl();
}

esp_err_t
esp_now_interop::register_slave_node(const uint8_t mac[ESP_NOW_ETH_ALEN]) {
  std::lock_guard<std::mutex> lock(this->state_mutex);
  if (!global_config::instance().is_master_node() || !this->discovery_mode) {
    ESP_LOGE("esp_now_interop", "Cannot register slave node on a non-master "
                                "node or when not in discover mode");
    return ESP_ERR_INVALID_ARG;
  }

  if (this->state.nodes_count >=
      esp_now_interop::ESP_NOW_INTEROP_SLAVE_NODES_MAX) {
    ESP_LOGE("esp_now_interop",
             "Cannot register more slave nodes, limit reached");
    return ESP_ERR_NO_MEM;
  }

  if (this->slave_node_exists_impl(mac)) {
    ESP_LOGW("esp_now_interop",
             "Attempted to register a slave node that already exists");
    return ESP_OK;
  }

  // add slave as permanent peer
  add_peer_unless_exists(mac);

  memcpy(
      this->state.nodes[this->state.nodes_count].mac,
      mac, ESP_NOW_ETH_ALEN);
  this->state.nodes_count++;
  // at least one slave node is registered, mark interop as configured, but
  // keep discovery on
  this->state.is_configured = true;
  return ESP_OK;
}

bool esp_now_interop::slave_node_exists_impl(
    const uint8_t mac[ESP_NOW_ETH_ALEN]) {
  if (!global_config::instance().is_master_node()) {
    ESP_LOGE("esp_now_interop",
             "Cannot check for slave node existence on a non-master node");
    return false;
  }

  for (size_t i = 0; i < this->state.nodes_count; i++) {
    if (memcmp(this->state.nodes[i].mac, mac, ESP_NOW_ETH_ALEN) ==
        0) {
      return true;
    }
  }
  return false;
}

bool esp_now_interop::slave_node_exists(const uint8_t mac[ESP_NOW_ETH_ALEN]) {
  std::lock_guard<std::mutex> lock(this->state_mutex);
  return this->slave_node_exists_impl(mac);
}
