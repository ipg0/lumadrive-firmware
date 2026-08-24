#include "ble_interop.hpp"
#include "../global_config/global_config.hpp"
#include "../mesh_interop/mesh_interop.hpp"
#include "esp_mac.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <nvs.h>

extern "C" {
void ble_store_config_init(void);
}

void ble_interop::nimble_host_task(void *param) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

int ble_interop::handle_ble_gap_event(struct ble_gap_event *event, void *arg) {
  std::lock_guard<std::mutex> lock(ble_interop::instance()._state_mutex);

  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    ESP_LOGI("ble_interop", "Received BLE GAP event of type CONNECT");
    if (event->connect.status == 0) {
      // Only enforce bonding check during bonded advertising mode
      if (ble_interop::instance()._is_bonded_advertising_active) {
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
        if (rc == 0) {
          // Check if this peer is in the bond store
          struct ble_store_key_sec key_sec;
          struct ble_store_value_sec value_sec;
          memset(&key_sec, 0, sizeof(key_sec));
          key_sec.peer_addr = desc.peer_id_addr;

          rc = ble_store_read_peer_sec(&key_sec, &value_sec);

          if (rc != 0) {
            // Peer not bonded; reject connection
            ESP_LOGW("ble_interop",
                     "Connection from non-bonded peer rejected: "
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
                     desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                     desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_UNSPECIFIED);
            return 0;
          }
        }
      }

      ESP_LOGI("ble_interop", "BLE connected (bonded), handle=%d",
               event->connect.conn_handle);
      ble_interop::instance()._is_connected = true;
      ble_interop::instance()._active_conn_handle = event->connect.conn_handle;
      ble_gap_security_initiate(event->connect.conn_handle);
    } else {
      ESP_LOGW("ble_interop", "BLE connection failed, status=%d",
               event->connect.status);
      ble_interop::instance()
          .start_advertising(); // resume advertising if connect failed
    }
    return 0;

  case BLE_GAP_EVENT_DISCONNECT:
    ESP_LOGI("ble_interop", "Received BLE GAP event of type DISCONNECT");
    ESP_LOGI("ble_interop", "BLE disconnected, reason=%d",
             event->disconnect.reason);
    ble_interop::instance()._is_connected = false;
    ble_interop::instance()._active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ble_interop::instance().start_bonded_advertising();
    return 0;

  case BLE_GAP_EVENT_ENC_CHANGE:
    ESP_LOGI("ble_interop", "Received BLE GAP event of type ENC_CHANGE");
    // fires after pairing/encryption completes (or fails)
    ESP_LOGI("ble_interop", "Encryption change, status=%d",
             event->enc_change.status);
    return 0;

  case BLE_GAP_EVENT_REPEAT_PAIRING: {
    ESP_LOGI("ble_interop", "Received BLE GAP event of type REPEAT_PAIRING");
    // phone tried to re-pair; typically means it lost its bond info.
    // Delete our stale bond and allow re-pairing.
    struct ble_gap_conn_desc desc;
    ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
    ble_store_util_delete_peer(&desc.peer_id_addr);
    return BLE_GAP_REPEAT_PAIRING_RETRY;
  }

  case BLE_GAP_EVENT_SUBSCRIBE:
    ESP_LOGI("ble_interop", "Received BLE GAP event of type SUBSCRIBE");
    ESP_LOGI("ble_interop", "Subscribe event: attr=%d notify=%d",
             event->subscribe.attr_handle, event->subscribe.cur_notify);
    return 0;

  default:
    return 0;
  }

  return 0;
}

void ble_interop::advertising_timer_cb(void *arg) {
  ble_interop::instance().stop_advertising();
  ble_interop::instance().start_bonded_advertising();
}

void ble_interop::stop_advertising() {
  if (!ble_gap_adv_active()) {
    return;
  }

  int rc = ble_gap_adv_stop();
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    ESP_LOGW("ble_interop", "ble_gap_adv_stop failed: %d", rc);
  }
}

ble_interop::ble_interop() {
  if (!global_config::instance().is_master_node()) {
    // slave nodes shouldn't have bluetooth interop
    return;
  }

  init_ble_device_name();

  ESP_ERROR_CHECK(nimble_port_init());
  ble_store_config_init();
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_gatts_count_cfg(this->gatt_service_defs);
  ble_gatts_add_svcs(this->gatt_service_defs);

  this->ble_hs_configure();
  this->_active_conn_handle = BLE_HS_CONN_HANDLE_NONE;

  static struct ble_npl_event g_mesh_status_event;

  ble_npl_event_init(&g_mesh_status_event, &ble_interop::notify_mesh_status,
                     NULL);

  mesh_interop::instance().register_mesh_state_cb([]() {
    if (!ble_npl_event_is_queued(&g_mesh_status_event)) {
      ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &g_mesh_status_event);
    }
  });

  const esp_timer_create_args_t advertising_timer_args = {
      .callback = &ble_interop::advertising_timer_cb,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "_advertising_timer",
      .skip_unhandled_events = true,
  };
  ESP_ERROR_CHECK(
      esp_timer_create(&advertising_timer_args, &this->_advertising_timer));

  nimble_port_freertos_init(&ble_interop::nimble_host_task);
}

void ble_interop::init_ble_device_name() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);

  snprintf(this->_ble_device_name, BLE_INTEROP_DEVICE_NAME_MAX_LENGTH,
           BLE_INTEROP_DEVICE_NAME_TEMPLATE, mac[3], mac[4], mac[5]);

  ESP_LOGI("ble_interop", "Initialized BLE device name: %s",
           this->_ble_device_name);
}

void ble_interop::ble_hs_configure() {
  // I have no fucking idea what most of this does
  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist =
      BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist =
      BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  ble_hs_cfg.sync_cb = ble_interop::ble_on_sync;
  ble_hs_cfg.reset_cb = ble_interop::ble_on_reset;
  ble_hs_cfg.store_status_cb =
      ble_store_util_status_rr; // default bond-storage handler

  ble_att_set_preferred_mtu(512);
}

void ble_interop::ble_on_sync() {
  std::lock_guard<std::mutex> lock(ble_interop::instance()._state_mutex);

  ESP_LOGI("ble_interop", "BLE host _synchronized");

  ble_addr_t addr;
  if (!load_or_generate_ble_address(&addr)) {
    ESP_LOGE("ble_interop", "Failed to load/generate BLE address");
    return;
  }

  int rc = ble_hs_id_set_rnd(addr.val);
  if (rc != 0) {
    ESP_LOGE("ble_interop", "Failed to set random address: %d", rc);
    return;
  }
  ble_svc_gap_device_name_set(ble_interop::instance()._ble_device_name);

  ble_interop::instance().start_advertising();
  instance()._is_ready = true;
}

void ble_interop::ble_on_reset(int reason) {
  std::lock_guard<std::mutex> lock(ble_interop::instance()._state_mutex);

  ESP_LOGE("ble_interop", "BLE host reset, reason: %d", reason);
  instance()._is_ready = false;
  instance()._is_connected = false;
}

void ble_interop::start_advertising() {
  struct ble_gap_adv_params adv_params;
  struct ble_hs_adv_fields adv_fields;
  struct ble_hs_adv_fields scan_rsp_fields;

  memset(&adv_fields, 0, sizeof(adv_fields));
  adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  adv_fields.tx_pwr_lvl_is_present = 1;
  adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
  adv_fields.uuids128 = (ble_uuid128_t *)&BLE_INTEROP_SERVICE_UUID;
  adv_fields.num_uuids128 = 1;
  adv_fields.uuids128_is_complete = 1;

  int fields_rc = ble_gap_adv_set_fields(&adv_fields);

  if (fields_rc != 0) {
    ESP_LOGE("ble_interop", "ble_gap_adv_set_fields failed: %d", fields_rc);
    return;
  }

  memset(&scan_rsp_fields, 0, sizeof(scan_rsp_fields));
  scan_rsp_fields.name = (uint8_t *)this->_ble_device_name;
  scan_rsp_fields.name_len = strlen(this->_ble_device_name);
  scan_rsp_fields.name_is_complete = 1;

  int scan_rsp_rc = ble_gap_adv_rsp_set_fields(&scan_rsp_fields);
  if (scan_rsp_rc != 0) {
    ESP_LOGE("ble_interop", "ble_gap_adv_rsp_set_fields failed: %d",
             scan_rsp_rc);
    return;
  }

  memset(&adv_params, 0, sizeof(adv_params));
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

  ESP_LOGI("ble_interop", "Starting BLE advertising with name: %s",
           this->_ble_device_name);

  ble_interop::instance()._is_bonded_advertising_active = false;

  int adv_rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, nullptr, BLE_HS_FOREVER,
                                 &adv_params, ble_interop::handle_ble_gap_event,
                                 nullptr);

  if (adv_rc != 0) {
    ESP_LOGE("ble_interop", "ble_gap_adv_start failed: %d", adv_rc);
  }

  esp_timer_start_once(ble_interop::_advertising_timer,
                       ble_interop::ADVERTISING_TIMEOUT_US);
}

void ble_interop::start_bonded_advertising() {
  ble_addr_t peer_addrs[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
  int num_peers = 0;

  if (this->_is_connected) {
    ESP_LOGI("ble_interop",
             "Skipping bonded advertising start while a connection is active");
    return;
  }

  this->stop_advertising();

  int rc = ble_store_util_bonded_peers(peer_addrs, &num_peers,
                                       MYNEWT_VAL(BLE_STORE_MAX_BONDS));

  if (rc != 0 || num_peers == 0) {
    ESP_LOGI("ble_interop", "No bonded peers, advertising stopped");
    return;
  }

  ESP_LOGI("ble_interop", "Found %d bonded peers, advertising to them",
           num_peers);

  // Debug: log each peer address and its type
  for (int i = 0; i < num_peers; i++) {
    const char *addr_type_str = peer_addrs[i].type == BLE_ADDR_RANDOM ? "RANDOM"
                                : peer_addrs[i].type == BLE_ADDR_PUBLIC
                                    ? "PUBLIC"
                                    : "UNKNOWN";
    ESP_LOGI("ble_interop",
             "Peer %d: addr_type=%s addr=%02x:%02x:%02x:%02x:%02x:%02x", i,
             addr_type_str, peer_addrs[i].val[5], peer_addrs[i].val[4],
             peer_addrs[i].val[3], peer_addrs[i].val[2], peer_addrs[i].val[1],
             peer_addrs[i].val[0]);
  }

  rc = ble_gap_wl_set(peer_addrs, num_peers);
  if (rc != 0) {
    // If a controller procedure was still winding down, make one retry.
    ESP_LOGW("ble_interop", "ble_gap_wl_set failed: %d; retrying once", rc);
    this->stop_advertising();
    rc = ble_gap_wl_set(peer_addrs, num_peers);
    if (rc != 0) {
      ESP_LOGE("ble_interop", "ble_gap_wl_set failed after retry: %d", rc);
      return;
    }
  }

  ESP_LOGI("ble_interop", "Whitelist successfully set with %d peers",
           num_peers);

  struct ble_hs_adv_fields adv_fields;
  memset(&adv_fields, 0, sizeof(adv_fields));
  // no DISC_GEN flag — not discoverable to new devices
  adv_fields.flags = BLE_HS_ADV_F_BREDR_UNSUP;
  adv_fields.tx_pwr_lvl_is_present = 1;
  adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
  rc = ble_gap_adv_set_fields(&adv_fields);
  if (rc != 0) {
    ESP_LOGE("ble_interop", "ble_gap_adv_set_fields (bonded) failed: %d", rc);
    return;
  }

  struct ble_gap_adv_params adv_params;
  memset(&adv_params, 0, sizeof(adv_params));
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_NON;
  adv_params.filter_policy =
      BLE_HCI_ADV_FILT_NONE; // whitelist filter for connections only; scans
                             // pass through

  ble_interop::instance()._is_bonded_advertising_active = true;

  rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, nullptr, BLE_HS_FOREVER,
                         &adv_params, ble_interop::handle_ble_gap_event,
                         nullptr);
  if (rc != 0) {
    ESP_LOGE("ble_interop", "ble_gap_adv_start (bonded) failed: %d", rc);
  }
}

bool ble_interop::load_or_generate_ble_address(ble_addr_t *out_addr) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open("ble_storage", NVS_READWRITE, &handle);
  if (err != ESP_OK)
    return false;

  size_t size = sizeof(out_addr->val);
  err = nvs_get_blob(handle, "ble_addr", out_addr->val, &size);

  if (err == ESP_OK && size == sizeof(out_addr->val)) {
    nvs_close(handle);
    return true; // successfully loaded a previously-saved address
  }

  // no saved address (first boot) — generate a new one and persist it
  ble_addr_t generated;
  int rc = ble_hs_id_gen_rnd(0, &generated);
  if (rc != 0) {
    nvs_close(handle);
    return false;
  }

  err = nvs_set_blob(handle, "ble_addr", generated.val, sizeof(generated.val));
  nvs_commit(handle);
  nvs_close(handle);

  if (err != ESP_OK)
    return false;

  memcpy(out_addr->val, generated.val, sizeof(generated.val));
  return true;
}