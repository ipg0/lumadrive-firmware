#pragma once

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include <mutex>

class ble_interop {
  static constexpr const char *BLE_INTEROP_DEVICE_NAME_TEMPLATE =
      "LUMAPROTO %02x-%02x-%02x";
  static constexpr size_t BLE_INTEROP_DEVICE_NAME_MAX_LENGTH = 32;

  static constexpr uint64_t ADVERTISING_TIMEOUT_US = 30 * 1000000; // 30 seconds

  static constexpr ble_uuid128_t BLE_INTEROP_SERVICE_UUID =
      BLE_UUID128_INIT(0x14, 0x32, 0x94, 0x32, 0x42, 0xc1, 0x4b, 0x48, 0xad,
                       0xb1, 0xce, 0x73, 0x43, 0xc5, 0x60, 0x71);

  static constexpr ble_uuid128_t BLE_INTEROP_MESH_STATUS_CHARACTERISTIC_UUID =
      BLE_UUID128_INIT(0x39, 0x15, 0x9d, 0x2c, 0x61, 0x5c, 0x41, 0xdc, 0x85,
                       0xbf, 0xd2, 0x57, 0x2d, 0x8b, 0x4b, 0x29);

private:
  static constexpr ble_uuid128_t MATRIX_UPLOAD_UUID =
      BLE_UUID128_INIT(0x39, 0x15, 0x9d, 0x2c, 0x61, 0x5c, 0x41, 0xdc, 0x85,
                      0xbf, 0xd2, 0x57, 0x2e, 0x8b, 0x4b, 0x29);
  static constexpr ble_uuid128_t MATRIX_STATUS_UUID =
      BLE_UUID128_INIT(0x39, 0x15, 0x9d, 0x2c, 0x61, 0x5c, 0x41, 0xdc, 0x85,
                      0xbf, 0xd2, 0x57, 0x2f, 0x8b, 0x4b, 0x29);
  static constexpr ble_uuid128_t LAYOUT_RESPONSE_UUID =
      BLE_UUID128_INIT(0x39, 0x15, 0x9d, 0x2c, 0x61, 0x5c, 0x41, 0xdc, 0x85,
                      0xbf, 0xd2, 0x57, 0x30, 0x8b, 0x4b, 0x29);
  static constexpr ble_uuid128_t OTA_UPLOAD_UUID =
      BLE_UUID128_INIT(0x39, 0x15, 0x9d, 0x2c, 0x61, 0x5c, 0x41, 0xdc, 0x85,
                      0xbf, 0xd2, 0x57, 0x31, 0x8b, 0x4b, 0x29);
  static constexpr ble_uuid128_t OTA_STATUS_UUID =
      BLE_UUID128_INIT(0x39, 0x15, 0x9d, 0x2c, 0x61, 0x5c, 0x41, 0xdc, 0x85,
                      0xbf, 0xd2, 0x57, 0x32, 0x8b, 0x4b, 0x29);
  static int handle_ota_upload(uint16_t, uint16_t, struct ble_gatt_access_ctxt *, void *);
  static int handle_ota_status(uint16_t, uint16_t, struct ble_gatt_access_ctxt *, void *);
  static uint16_t _matrix_status_handle;
  static int handle_matrix_upload(uint16_t, uint16_t, struct ble_gatt_access_ctxt *, void *);
  static int handle_matrix_status(uint16_t, uint16_t, struct ble_gatt_access_ctxt *, void *);
  static int handle_layout_response(uint16_t, uint16_t, struct ble_gatt_access_ctxt *, void *);
  static constexpr const char *TAG = "ble_interop";
  std::mutex _state_mutex;
  bool _is_ready = false;
  bool _is_connected = false;
  bool _is_bonded_advertising_active = false;
  char _ble_device_name[BLE_INTEROP_DEVICE_NAME_MAX_LENGTH];
  esp_timer_handle_t _advertising_timer;
  uint16_t _active_conn_handle;

  static void advertising_timer_cb(void *arg);
  static int handle_ble_gap_event(struct ble_gap_event *event, void *arg);
  static void ble_on_sync(void);
  static void ble_on_reset(int reason);
  static void nimble_host_task(void *param);
  static void ble_hs_configure();
  void init_ble_device_name();
  void start_advertising();
  void stop_advertising();
  void start_bonded_advertising();
  static bool load_or_generate_ble_address(ble_addr_t *out_addr);

  ble_interop();
  ~ble_interop() = default;

  // handlers
  static int
  handle_ble_gap_mesh_status_access(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt,
                                    void *arg);

// unneeded initializers in gatt_service_defs
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

  static constexpr struct ble_gatt_svc_def GATT_SERVICE_DEFS[] = {
      {
          .type = BLE_GATT_SVC_TYPE_PRIMARY,
          .uuid = &BLE_INTEROP_SERVICE_UUID.u,
          .characteristics =
              (struct ble_gatt_chr_def[]){
                  {
                      .uuid = &BLE_INTEROP_MESH_STATUS_CHARACTERISTIC_UUID.u,
                      .access_cb =
                          ble_interop::handle_ble_gap_mesh_status_access,
                      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY |
                               BLE_GATT_CHR_F_READ_ENC |
                               BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
                  },
                  {
                      .uuid = &MATRIX_UPLOAD_UUID.u,
                      .access_cb = ble_interop::handle_matrix_upload,
                      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
                  },
                  {
                      .uuid = &MATRIX_STATUS_UUID.u,
                      .access_cb = ble_interop::handle_matrix_status,
                      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY |
                               BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
                      .val_handle = &_matrix_status_handle,
                  },
                  {
                      .uuid = &LAYOUT_RESPONSE_UUID.u,
                      .access_cb = ble_interop::handle_layout_response,
                      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
                  },
                  {
                      .uuid = &OTA_UPLOAD_UUID.u,
                      .access_cb = ble_interop::handle_ota_upload,
                      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
                  },
                  {
                      .uuid = &OTA_STATUS_UUID.u,
                      .access_cb = ble_interop::handle_ota_status,
                      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
                  },
                  {0}, // end of characteristics
              },
      },
      {0}, // end of services
  };

#pragma GCC diagnostic pop

public:
  static ble_interop &instance() {
    static ble_interop instance;
    return instance;
  }

  static void notify_mesh_status(struct ble_npl_event *ev);
  static void init_matrix_notifications();
};
