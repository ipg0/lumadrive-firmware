#include "ble_interop.hpp"
#include "../global_config/global_config.hpp"
#include "esp_mac.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <nvs.h>

void ble_interop::nimble_host_task(void *param) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

int ble_interop::handle_ble_gap_event(struct ble_gap_event *event, void *arg) {
  std::lock_guard<std::mutex> lock(ble_interop::instance().state_mutex);

  ESP_LOGI("ble_interop", "Received BLE GAP event of type %d", event->type);
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    if (event->connect.status == 0) {
      ESP_LOGI("ble_interop", "BLE connected, handle=%d",
               event->connect.conn_handle);
      ble_interop::instance().is_connected = true;
    } else {
      ESP_LOGW("ble_interop", "BLE connection failed, status=%d",
               event->connect.status);
      ble_interop::instance()
          .start_advertising(); // resume advertising if connect failed
    }
    return 0;

  case BLE_GAP_EVENT_DISCONNECT:
    ESP_LOGI("ble_interop", "BLE disconnected, reason=%d",
             event->disconnect.reason);
    ble_interop::instance().is_connected = false;
    ble_interop::instance()
        .start_advertising(); // resume advertising so phone can reconnect
    return 0;

  case BLE_GAP_EVENT_ENC_CHANGE:
    // fires after pairing/encryption completes (or fails)
    ESP_LOGI("ble_interop", "Encryption change, status=%d",
             event->enc_change.status);
    return 0;

  case BLE_GAP_EVENT_REPEAT_PAIRING: {
    // phone tried to re-pair; typically means it lost its bond info.
    // Delete our stale bond and allow re-pairing.
    struct ble_gap_conn_desc desc;
    ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
    ble_store_util_delete_peer(&desc.peer_id_addr);
    return BLE_GAP_REPEAT_PAIRING_RETRY;
  }

  case BLE_GAP_EVENT_SUBSCRIBE:
    ESP_LOGI("ble_interop", "Subscribe event: attr=%d notify=%d",
             event->subscribe.attr_handle, event->subscribe.cur_notify);
    return 0;

  default:
    return 0;
  }

  return 0;
}

int ble_interop::handle_ble_gap_status_event(uint16_t conn_handle,
                                             uint16_t attr_handle,
                                             struct ble_gatt_access_ctxt *ctxt,
                                             void *arg) {
  ESP_LOGI("ble_interop",
           "Received BLE GAP status event for conn_handle=%d attr_handle=%d",
           conn_handle, attr_handle);
  return 0;
}

int ble_interop::handle_ble_gap_command_event(uint16_t conn_handle,
                                              uint16_t attr_handle,
                                              struct ble_gatt_access_ctxt *ctxt,
                                              void *arg) {
  ESP_LOGI("ble_interop",
           "Received BLE GAP command event for conn_handle=%d attr_handle=%d",
           conn_handle, attr_handle);
  return 0;
}

void ble_interop::advertising_timer_cb(void *arg) { ble_gap_adv_stop(); }

ble_interop::ble_interop() {
  if (!global_config::instance().is_master_node()) {
    // slave nodes shouldn't have bluetooth interop
    return;
  }

  init_ble_device_name();

  ESP_ERROR_CHECK(nimble_port_init());
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_gatts_count_cfg(this->gatt_service_defs);
  ble_gatts_add_svcs(this->gatt_service_defs);

  this->ble_hs_configure();

  const esp_timer_create_args_t advertising_timer_args = {
      .callback = &ble_interop::advertising_timer_cb,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "advertising_timer",
      .skip_unhandled_events = true,
  };
  ESP_ERROR_CHECK(
      esp_timer_create(&advertising_timer_args, &this->advertising_timer));

  ble_store_clear(); // TODO: for test consistency, remove later
  nimble_port_freertos_init(&ble_interop::nimble_host_task);
}

void ble_interop::init_ble_device_name() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);

  snprintf(this->ble_device_name, BLE_INTEROP_DEVICE_NAME_MAX_LENGTH,
           BLE_INTEROP_DEVICE_NAME_TEMPLATE, mac[3], mac[4], mac[5]);

  ESP_LOGI("ble_interop", "Initialized BLE device name: %s",
           this->ble_device_name);
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
}

void ble_interop::ble_on_sync() {
  std::lock_guard<std::mutex> lock(ble_interop::instance().state_mutex);

  ESP_LOGI("ble_interop", "BLE host synchronized");

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
  ble_svc_gap_device_name_set(ble_interop::instance().ble_device_name);

  ble_interop::instance().start_advertising();
  instance().is_ready = true;
}

void ble_interop::ble_on_reset(int reason) {
  std::lock_guard<std::mutex> lock(ble_interop::instance().state_mutex);

  ESP_LOGE("ble_interop", "BLE host reset, reason: %d", reason);
  instance().is_ready = false;
  instance().is_connected = false;
}

void ble_interop::start_advertising() {
  struct ble_gap_adv_params adv_params;
  struct ble_hs_adv_fields fields;

  memset(&fields, 0, sizeof(fields));
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.tx_pwr_lvl_is_present = 1;
  fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
  fields.name = (uint8_t *)this->ble_device_name;
  fields.name_len = strlen(this->ble_device_name);
  fields.name_is_complete = 1;

  int fields_rc = ble_gap_adv_set_fields(&fields);

  if (fields_rc != 0) {
    ESP_LOGE("ble_interop", "ble_gap_adv_set_fields failed: %d", fields_rc);
  }

  memset(&adv_params, 0, sizeof(adv_params));
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

  ESP_LOGI("ble_interop", "Starting BLE advertising with name: %s",
           this->ble_device_name);

  int adv_rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, nullptr, BLE_HS_FOREVER,
                                 &adv_params, ble_interop::handle_ble_gap_event,
                                 nullptr);

  if (adv_rc != 0) {
    ESP_LOGE("ble_interop", "ble_gap_adv_start failed: %d", adv_rc);
  }

  esp_timer_start_once(ble_interop::advertising_timer, 60000000); // 10 seconds
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