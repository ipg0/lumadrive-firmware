#include "mesh_interop.hpp"
#include "../global_config/global_config.hpp"
#include "mesh_interop_frame/mesh_interop_frame.hpp"
#include "mesh_interop_frame/impl/mesh_interop_frame_discover.hpp"
#include "mesh_interop_frame/impl/mesh_interop_frame_heartbeat.hpp"
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs.h>
#include "packet_router.hpp"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace {
struct received_frame {
  uint8_t mac[6];
  uint8_t data[ESP_NOW_MAX_DATA_LEN];
  size_t size;
  int64_t received_at;
};
QueueHandle_t mesh_receive_queue = nullptr;
void mesh_receive_task(void*) {
  received_frame packet;
  for (;;) {
    if (xQueueReceive(mesh_receive_queue, &packet, portMAX_DELAY) != pdTRUE)
      continue;
    auto* frame = mesh_interop_frame_base::parse(packet.data, packet.size, packet.mac);
    if (!frame)
      continue;
    frame->received_at_us = packet.received_at;
    frame->handle();
    delete frame;
  }
}
} // namespace

void mesh_interop::handle_esp_now_recv(const esp_now_recv_info_t* esp_now_info, const uint8_t* data,
                                       int data_len) {
  const auto received_at = esp_timer_get_time();
  if (data_len <= 0 || data_len > ESP_NOW_MAX_DATA_LEN)
    return;
  if (mesh_packet_router().dispatch(esp_now_info->src_addr, data, data_len))
    return;
  received_frame packet;
  memcpy(packet.mac, esp_now_info->src_addr, 6);
  memcpy(packet.data, data, data_len);
  packet.size = data_len;
  packet.received_at = received_at;
  if (xQueueSend(mesh_receive_queue, &packet, 0) != pdTRUE)
    ESP_EARLY_LOGW("mesh_interop", "Receive queue full");
}

void mesh_interop::heartbeat_timer_cb(void* arg) {
  if (!global_config::instance().is_master_node())
    return; // timer shouldn't exist on non-master node

  mesh_interop& interop = mesh_interop::instance();

  const int64_t now = esp_timer_get_time();
  if (now - interop._startup_at < STARTUP_WAKE_US)
    interop.send_heartbeats();
  else if (now - interop._last_periodic_heartbeat >= MESH_INTEROP_HEARTBEAT_INTERVAL_US) {
    interop.send_heartbeats();
    interop._last_periodic_heartbeat = now;
  }
  if (interop.is_discovery_mode() &&
      now - interop._last_discovery_broadcast >= MESH_INTEROP_HEARTBEAT_INTERVAL_US) {
    interop.broadcast_discover();
    interop._last_discovery_broadcast = now;
  }
}

mesh_interop::mesh_interop() {
  const wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_ERROR_CHECK(esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE));

  ESP_ERROR_CHECK(esp_now_init());
  mesh_receive_queue = xQueueCreate(16, sizeof(received_frame));
  ESP_ERROR_CHECK(mesh_receive_queue ? ESP_OK : ESP_ERR_NO_MEM);
  ESP_ERROR_CHECK(xTaskCreate(mesh_receive_task, "mesh_rx", 6144, nullptr, 6, nullptr) == pdPASS
                      ? ESP_OK
                      : ESP_ERR_NO_MEM);
  ESP_ERROR_CHECK(esp_now_register_recv_cb(&mesh_interop::handle_esp_now_recv));

  // add broadcast MAC as peer by default
  if (global_config::instance().is_master_node()) {
    esp_now_peer_info_t broadcast_peer_info = {};
    memset(&broadcast_peer_info, 0, sizeof(broadcast_peer_info));
    broadcast_peer_info.channel = 6;
    broadcast_peer_info.encrypt = false;
    memcpy(broadcast_peer_info.peer_addr, "\xFF\xFF\xFF\xFF\xFF\xFF", ESP_NOW_ETH_ALEN);

    ESP_ERROR_CHECK(esp_now_add_peer(&broadcast_peer_info));
  }

  gpio_config_t discovery_gpio_conf = {
      .pin_bit_mask = (1ULL << CONFIG_DISCOVERY_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };

  ESP_ERROR_CHECK(gpio_config(&discovery_gpio_conf));

  bool force_discovery = gpio_get_level(CONFIG_DISCOVERY_GPIO) == 0;

  if (force_discovery) {
    ESP_LOGW("mesh_interop", "Discovery GPIO is LOW, forcing discovery mode on boot");
    this->start_discovery_impl();
  } else {
    esp_err_t load_state_err = this->load_state();

    if (load_state_err != ESP_OK) {
      ESP_LOGW("mesh_interop", "Failed to load _state from NVS: %s",
               esp_err_to_name(load_state_err));
      this->init_state();
    }

    if (!this->is_interop_configured()) {
      this->start_discovery_impl();
    } else {
      ESP_LOGI("mesh_interop", "Interop _state loaded, is_configured: %s",
               this->_state.is_configured ? "true" : "false");
      if (global_config::instance().is_master_node()) {
        ESP_LOGI("mesh_interop", "Master node, slave nodes count: %u", this->_state.nodes_count);
      } else {
        ESP_LOGI("mesh_interop", "Slave node, master node MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 this->_state.nodes[0].mac[0], this->_state.nodes[0].mac[1],
                 this->_state.nodes[0].mac[2], this->_state.nodes[0].mac[3],
                 this->_state.nodes[0].mac[4], this->_state.nodes[0].mac[5]);
      }
    }
  }

  this->_mesh_state_cb = nullptr;

  if (global_config::instance().is_master_node()) {
    // start heartbeat timer
    const esp_timer_create_args_t heartbeat_timer_args = {
        .callback = &mesh_interop::heartbeat_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "heartbeat_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t heartbeat_timer;
    ESP_ERROR_CHECK(esp_timer_create(&heartbeat_timer_args, &heartbeat_timer));
    _startup_at = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_timer_start_periodic(heartbeat_timer, STARTUP_HEARTBEAT_INTERVAL_US));
  }
}

bool mesh_interop::startup_waiting_for_nodes(uint64_t now) {
  std::lock_guard<std::mutex> lock(_state_mutex);
  if (now >= uint64_t(_startup_at) + STARTUP_WAKE_US)
    return false;
  for (size_t i = 0; i < _state.nodes_count; ++i)
    if (!_node_last_synced_at[i])
      return true;
  return false;
}

esp_err_t mesh_interop::set_standby_radio(bool standby) {
  auto err = esp_now_set_wake_window(standby ? 40 : UINT16_MAX);
  if (err != ESP_OK)
    return err;
  err = esp_wifi_connectionless_module_set_wake_interval(
      standby ? 200 : ESP_WIFI_CONNECTIONLESS_INTERVAL_DEFAULT_MODE);
  if (err != ESP_OK && standby)
    esp_now_set_wake_window(UINT16_MAX);
  return err;
}

void mesh_interop::finish_discovery_timer_cb(void* arg) {
  mesh_interop::instance().finish_discovery();
}

esp_err_t mesh_interop::start_discovery_impl() {
  this->_discovery_mode = true;

  const esp_err_t clear_err = this->clear_saved_state();
  if (clear_err != ESP_OK) {
    ESP_LOGW("mesh_interop", "Failed to clear saved mesh state: %s", esp_err_to_name(clear_err));
  }
  // Discovery must begin with the current role even if there was no NVS key,
  // or erasing it failed. Otherwise a fresh master's zero-initialized role
  // is saved as a slave and cannot be restored on the next boot.
  this->init_state();

  if (global_config::instance().is_master_node()) {
    this->_state.is_configured = false;
    this->_state.nodes_count = 0;

    const esp_timer_create_args_t finish_discovery_timer_args = {
        .callback = &mesh_interop::finish_discovery_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "finish_discovery_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t finish_discovery_timer;
    ESP_ERROR_CHECK(esp_timer_create(&finish_discovery_timer_args, &finish_discovery_timer));
    ESP_ERROR_CHECK(
        esp_timer_start_once(finish_discovery_timer, MESH_INTEROP_MASTER_DISCOVERY_TIMEOUT_US));
  } else {
    this->_state.is_configured = false;
  }

  this->notify_mesh_state_cb();

  return ESP_OK;
}

esp_err_t mesh_interop::start_discovery() {
  std::lock_guard<std::mutex> lock(this->_state_mutex);

  if (this->_discovery_mode) {
    ESP_LOGW("mesh_interop", "start_discovery called while already in discovery mode");
    return ESP_OK;
  }
  return this->start_discovery_impl();
}

esp_err_t mesh_interop::finish_discovery_impl() {
  this->_discovery_mode = false;

  if (global_config::instance().is_master_node()) {
    ESP_LOGI("mesh_interop",
             "Discovery finished, %u slave nodes registered:", this->_state.nodes_count);

    for (size_t i = 0; i < this->_state.nodes_count; i++) {
      const auto& slave_node = this->_state.nodes[i];
      ESP_LOGI("mesh_interop", "Slave node %u: %02X:%02X:%02X:%02X:%02X:%02X", i, slave_node.mac[0],
               slave_node.mac[1], slave_node.mac[2], slave_node.mac[3], slave_node.mac[4],
               slave_node.mac[5]);
    }
  } else {
    ESP_LOGI("mesh_interop", "Discovery finished, master node: %02X:%02X:%02X:%02X:%02X:%02X",
             this->_state.nodes[0].mac[0], this->_state.nodes[0].mac[1],
             this->_state.nodes[0].mac[2], this->_state.nodes[0].mac[3],
             this->_state.nodes[0].mac[4], this->_state.nodes[0].mac[5]);
  }

  if (this->_state.is_configured) {
    ESP_LOGI("mesh_interop", "Interop is configured, saving _state to NVS");
    const esp_err_t save_err = this->save_state();
    if (save_err != ESP_OK) {
      ESP_LOGE("mesh_interop", "Failed to save mesh state: %s", esp_err_to_name(save_err));
    }
  } else {
    ESP_LOGW("mesh_interop", "Interop is not configured, not saving _state in NVS, reboot!");
  }

  this->notify_mesh_state_cb();

  return ESP_OK;
}

esp_err_t mesh_interop::finish_discovery() {
  std::lock_guard<std::mutex> lock(this->_state_mutex);
  if (!this->_discovery_mode) {
    ESP_LOGW("mesh_interop", "finish_discovery called while not in discovery mode");
    return ESP_OK;
  }
  return this->finish_discovery_impl();
}

void mesh_interop::broadcast_discover() {
  mesh_interop_frame<mesh_interop_frame_type::DISCOVER> discover_frame;
  esp_err_t send_err = discover_frame.send();
  if (send_err != ESP_OK) {
    ESP_LOGE("mesh_interop", "Failed to send discover frame: %s", esp_err_to_name(send_err));
  }
}

void mesh_interop::send_heartbeats() {
  // Take a snapshot so discovery and subscription cannot change the list while sending.
  if (!global_config::instance().is_master_node())
    return; // only master node should send heartbeat

  mesh_interop_state_t state{};
  {
    std::lock_guard<std::mutex> lock(_state_mutex);
    state = _state;
  }
  for (size_t i = 0; i < state.nodes_count; i++) {
    const auto& slave_node = state.nodes[i];

    mesh_interop_frame<mesh_interop_frame_type::HEARTBEAT> heartbeat_frame(slave_node.mac);
    esp_err_t send_err = heartbeat_frame.send();
    if (send_err != ESP_OK) {
      ESP_LOGE("mesh_interop", "Failed to send heartbeat frame to slave node %u: %s", i,
               esp_err_to_name(send_err));
    }

    // TODO: reply timeouts & track missing nodes
  }
}

bool mesh_interop::is_interop_configured() {
  std::lock_guard<std::mutex> lock(this->_state_mutex);

  if (global_config::instance().is_master_node()) {
    return this->_state.is_configured && this->_state.nodes_count > 0;
  } else {
    return this->_state.is_configured;
  }
}

bool mesh_interop::add_peer_unless_exists(const uint8_t mac[ESP_NOW_ETH_ALEN]) {
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
    ESP_LOGE("mesh_interop", "Failed to add peer: %s", esp_err_to_name(add_peer_err));
    return false;
  }

  return true;
}

void mesh_interop::remove_peer(const uint8_t mac[ESP_NOW_ETH_ALEN]) {
  esp_err_t remove_peer_err = esp_now_del_peer(mac);
  if (remove_peer_err != ESP_OK && remove_peer_err != ESP_ERR_ESPNOW_NOT_FOUND) {
    ESP_LOGE("mesh_interop", "Failed to remove peer: %s", esp_err_to_name(remove_peer_err));
  }
}

esp_err_t mesh_interop::subscribe_to_master_node(const uint8_t mac[ESP_NOW_ETH_ALEN]) {
  std::lock_guard<std::mutex> lock(this->_state_mutex);
  if (global_config::instance().is_master_node()) {
    ESP_LOGE("mesh_interop", "Cannot subscribe to master node on a master node");
    return ESP_ERR_INVALID_ARG;
  }

  this->_state.is_configured = true;
  this->_state.nodes_count = 1;
  memcpy(this->_state.nodes[0].mac, mac, ESP_NOW_ETH_ALEN);

  // add master as permanent peer
  add_peer_unless_exists(mac);

  return this->finish_discovery_impl();
}

esp_err_t mesh_interop::register_slave_node(const uint8_t mac[ESP_NOW_ETH_ALEN]) {
  std::lock_guard<std::mutex> lock(this->_state_mutex);
  if (!global_config::instance().is_master_node() || !this->_discovery_mode) {
    ESP_LOGE("mesh_interop", "Cannot register slave node on a non-master "
                             "node or when not in discover mode");
    return ESP_ERR_INVALID_ARG;
  }

  if (this->_state.nodes_count >= mesh_interop::MESH_INTEROP_SLAVE_NODES_MAX) {
    ESP_LOGE("mesh_interop", "Cannot register more slave nodes, limit reached");
    return ESP_ERR_NO_MEM;
  }

  if (this->find_slave_node_index(mac) != SIZE_MAX) {
    ESP_LOGW("mesh_interop", "Attempted to register a slave node that already exists");
    return ESP_OK;
  }

  // add slave as permanent peer
  add_peer_unless_exists(mac);

  memcpy(this->_state.nodes[this->_state.nodes_count].mac, mac, ESP_NOW_ETH_ALEN);
  this->_state.nodes_count++;
  // at least one slave node is registered, mark interop as configured, but
  // keep discovery on
  this->_state.is_configured = true;

  this->notify_mesh_state_cb();

  return ESP_OK;
}

bool mesh_interop::master_node_matches_impl(const uint8_t mac[ESP_NOW_ETH_ALEN]) {
  if (global_config::instance().is_master_node()) {
    ESP_LOGE("mesh_interop", "Cannot check for master node match on a master node");
    return false;
  }

  return memcmp(this->_state.nodes[0].mac, mac, ESP_NOW_ETH_ALEN) == 0;
}

bool mesh_interop::slave_node_exists(const uint8_t mac[ESP_NOW_ETH_ALEN]) {
  std::lock_guard<std::mutex> lock(this->_state_mutex);
  return this->find_slave_node_index(mac) != SIZE_MAX;
}

size_t mesh_interop::find_slave_node_index(const uint8_t mac[ESP_NOW_ETH_ALEN]) {
  if (!global_config::instance().is_master_node()) {
    ESP_LOGE("mesh_interop", "Cannot check for slave node existence on a non-master node");
    return SIZE_MAX;
  }

  for (size_t i = 0; i < this->_state.nodes_count; i++) {
    if (memcmp(this->_state.nodes[i].mac, mac, ESP_NOW_ETH_ALEN) == 0) {
      return i;
    }
  }
  return SIZE_MAX; // not found
}

esp_err_t mesh_interop::sync_slave_node(const uint8_t mac[ESP_NOW_ETH_ALEN]) {
  std::lock_guard<std::mutex> lock(this->_state_mutex);
  if (!global_config::instance().is_master_node()) {
    ESP_LOGE("mesh_interop", "Cannot sync slave node on a slave node");
    return ESP_ERR_INVALID_ARG;
  }

  size_t slave_node_index = this->find_slave_node_index(mac);

  if (slave_node_index == SIZE_MAX) {
    ESP_LOGE("mesh_interop", "Cannot sync slave node that is not registered as a slave node");
    return ESP_ERR_INVALID_ARG;
  }

  this->_node_last_synced_at[slave_node_index] = esp_timer_get_time();
  ESP_LOGI("mesh_interop", "Synced slave node %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);

  this->notify_mesh_state_cb();

  return ESP_OK;
}

bool mesh_interop::master_node_matches(const uint8_t mac[ESP_NOW_ETH_ALEN]) {
  std::lock_guard<std::mutex> lock(this->_state_mutex);
  return this->master_node_matches_impl(mac);
}

esp_err_t mesh_interop::load_state() {
  nvs_handle_t nvs_handle;
  esp_err_t nvs_err = nvs_open("mesh_interop", NVS_READONLY, &nvs_handle);
  if (nvs_err != ESP_OK) {
    ESP_LOGE("mesh_interop", "Failed to open NVS namespace: %s", esp_err_to_name(nvs_err));
    return nvs_err;
  }

  size_t size = sizeof(this->_state);
  nvs_err = nvs_get_blob(nvs_handle, "_state", &this->_state, &size);
  nvs_close(nvs_handle);

  if (nvs_err != ESP_OK) {
    ESP_LOGE("mesh_interop", "Failed to read _state from NVS: %s", esp_err_to_name(nvs_err));
    return nvs_err;
  }

  if (size != sizeof(this->_state)) {
    ESP_LOGE("mesh_interop", "State size mismatch, expected %zu, got %zu", sizeof(this->_state),
             size);
    return ESP_ERR_INVALID_SIZE;
  }

  if (this->_state.magic != mesh_interop::MESH_INTEROP_NVS_MAGIC) {
    ESP_LOGE("mesh_interop",
             "State magic mismatch, expected 0x%08X, got "
             "0x%08X",
             mesh_interop::MESH_INTEROP_NVS_MAGIC, this->_state.magic);
    return ESP_ERR_INVALID_STATE;
  }

  if (this->_state.is_master_state != global_config::instance().is_master_node()) {
    ESP_LOGE("mesh_interop", "State master/slave mismatch, expected %s, got %s",
             global_config::instance().is_master_node() ? "master" : "slave",
             this->_state.is_master_state ? "master" : "slave");
    return ESP_ERR_INVALID_STATE;
  }

  if (this->_state.nodes_count > MESH_INTEROP_SLAVE_NODES_MAX ||
      (!this->_state.is_master_state && this->_state.is_configured &&
       this->_state.nodes_count != 1)) {
    ESP_LOGE("mesh_interop", "Invalid saved mesh node count: %u", this->_state.nodes_count);
    return ESP_ERR_INVALID_STATE;
  }

  for (size_t i = 0; i < MESH_INTEROP_SLAVE_NODES_MAX; i++) {
    this->_node_last_synced_at[i] = 0;
  }

  return this->add_all_state_nodes_as_peers();
}

esp_err_t mesh_interop::save_state() {
  nvs_handle_t nvs_handle;
  esp_err_t nvs_err = nvs_open("mesh_interop", NVS_READWRITE, &nvs_handle);
  if (nvs_err != ESP_OK) {
    ESP_LOGE("mesh_interop", "Failed to open NVS namespace: %s", esp_err_to_name(nvs_err));
    return nvs_err;
  }

  this->_state.magic = mesh_interop::MESH_INTEROP_NVS_MAGIC;
  this->_state.is_master_state = global_config::instance().is_master_node();

  nvs_err = nvs_set_blob(nvs_handle, "_state", &this->_state, sizeof(this->_state));
  if (nvs_err != ESP_OK) {
    ESP_LOGE("mesh_interop", "Failed to write _state to NVS: %s", esp_err_to_name(nvs_err));
    nvs_close(nvs_handle);
    return nvs_err;
  }

  nvs_err = nvs_commit(nvs_handle);
  if (nvs_err != ESP_OK) {
    ESP_LOGE("mesh_interop", "Failed to commit _state to NVS: %s", esp_err_to_name(nvs_err));
    nvs_close(nvs_handle);
    return nvs_err;
  }

  nvs_close(nvs_handle);
  return ESP_OK;
}

esp_err_t mesh_interop::clear_saved_state() {
  nvs_handle_t nvs_handle;
  esp_err_t nvs_err = nvs_open("mesh_interop", NVS_READWRITE, &nvs_handle);
  if (nvs_err != ESP_OK) {
    ESP_LOGE("mesh_interop", "Failed to open NVS namespace: %s", esp_err_to_name(nvs_err));
    return nvs_err;
  }

  nvs_err = nvs_erase_key(nvs_handle, "_state");
  if (nvs_err == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(nvs_handle);
    return ESP_OK;
  }
  if (nvs_err != ESP_OK) {
    ESP_LOGE("mesh_interop", "Failed to erase _state from NVS: %s", esp_err_to_name(nvs_err));
    nvs_close(nvs_handle);
    return nvs_err;
  }

  nvs_err = nvs_commit(nvs_handle);
  if (nvs_err != ESP_OK) {
    ESP_LOGE("mesh_interop", "Failed to commit erase _state to NVS: %s", esp_err_to_name(nvs_err));
    nvs_close(nvs_handle);
    return nvs_err;
  }

  nvs_close(nvs_handle);

  return ESP_OK;
}

void mesh_interop::init_state() {
  this->_state.magic = mesh_interop::MESH_INTEROP_NVS_MAGIC;
  this->_state.is_configured = false;
  this->_state.is_master_state = global_config::instance().is_master_node();
  this->_state.nodes_count = 0;

  for (size_t i = 0; i < MESH_INTEROP_SLAVE_NODES_MAX; i++) {
    this->_node_last_synced_at[i] = 0;
  }
}

esp_err_t mesh_interop::add_all_state_nodes_as_peers() {
  if (!this->is_interop_configured()) {
    ESP_LOGE("mesh_interop", "Cannot add all _state nodes as peers, interop is not configured");
    return ESP_ERR_INVALID_STATE;
  }

  if (global_config::instance().is_master_node()) {
    for (size_t i = 0; i < this->_state.nodes_count; i++) {
      const auto& slave_node = this->_state.nodes[i];
      this->add_peer_unless_exists(slave_node.mac);
    }
  } else {
    this->add_peer_unless_exists(this->_state.nodes[0].mac);
  }

  return ESP_OK;
}

mesh_interop::mesh_interop_state_result_t mesh_interop::get_state() {
  std::lock_guard<std::mutex> lock(this->_state_mutex);
  mesh_interop_state_result_t result;

  result.is_discovery_mode = this->_discovery_mode;

  if (result.is_discovery_mode) {
    result.nodes_count = 0;
    return result;
  }

  result.nodes_count = this->_state.nodes_count;

  for (size_t i = 0; i < this->_state.nodes_count; i++) {
    const auto& slave_node = this->_state.nodes[i];
    memcpy(result.nodes[i].mac, slave_node.mac, ESP_NOW_ETH_ALEN);
    result.nodes[i].last_synced_at = this->_node_last_synced_at[i];
  }

  return result;
}

void mesh_interop::register_mesh_state_cb(void (*cb)()) {
  std::lock_guard<std::mutex> lock(this->_state_mutex);
  if (cb == nullptr) {
    ESP_LOGE("mesh_interop", "Cannot register null mesh _state callback");
    return;
  }

  this->_mesh_state_cb = cb;
}

void mesh_interop::notify_mesh_state_cb() {
  if (this->_mesh_state_cb != nullptr) {
    this->_mesh_state_cb();
  }
}
