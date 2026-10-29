#include "../../master_clock_manager/master_clock_manager.hpp"
#include "../../mesh_interop/mesh_interop.hpp"
#include "../ble_interop.hpp"
#include <mesh.pb.h>
#include <pb_encode.h>

namespace {
esp_err_t write_mesh_interop_dto(pb_ostream_t *stream) {
  auto mesh_state_result = mesh_interop::instance().get_state();
  auto time_now = master_clock_manager::instance().get_master_time();
  mesh_interop_MeshInteropState mesh_state =
      mesh_interop_MeshInteropState_init_zero;
  mesh_state.is_discovery_mode = mesh_state_result.is_discovery_mode;
  mesh_state.nodes_count = mesh_state_result.nodes_count;
  for (size_t i = 0; i < mesh_state_result.nodes_count; i++) {
    mesh_state.nodes[i] = mesh_interop_MeshInteropNode_init_zero;
    mesh_state.nodes[i].last_synced_at =
        mesh_state_result.nodes[i].last_synced_at == 0
            ? 0
            : time_now - mesh_state_result.nodes[i].last_synced_at;
    memcpy(mesh_state.nodes[i].mac, mesh_state_result.nodes[i].mac,
           sizeof(mesh_state.nodes[i].mac));
  }

  if (!pb_encode(stream, mesh_interop_MeshInteropState_fields, &mesh_state)) {
    ESP_LOGE("ble_interop", "Failed to encode mesh _state: %s",
             PB_GET_ERROR(stream));
    return ESP_FAIL;
  }

  // Proto3 omits every field once discovery ends with no paired nodes. Keep
  // the characteristic readable for clients that reject a zero-length value.
  if (!stream->bytes_written &&
      (!pb_encode_tag(stream, PB_WT_VARINT, mesh_interop_MeshInteropState_is_discovery_mode_tag) ||
       !pb_encode_varint(stream, 0))) return ESP_FAIL;

  return ESP_OK;
}
} // namespace

int ble_interop::handle_ble_gap_mesh_status_access(
    uint16_t conn_handle, uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt, void *arg) {
  ESP_LOGI("ble_interop",
           "Received BLE GAP status event for conn_handle=%d attr_handle=%d",
           conn_handle, attr_handle);

  switch (ctxt->op) {
  case BLE_GATT_ACCESS_OP_READ_CHR: {
    ESP_LOGI("ble_interop", "Handling BLE GAP status read request");

    uint8_t buffer[mesh_interop_MeshInteropState_size];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));

    if (write_mesh_interop_dto(&stream) != ESP_OK) {
      return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    ESP_LOGI("ble_interop", "Encoded mesh _state with size %zu bytes",
             stream.bytes_written);

    if (os_mbuf_append(ctxt->om, buffer, stream.bytes_written) != 0) {
      ESP_LOGE("ble_interop", "Failed to append data to mbuf");
      return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    break;
  }
  default:
    ESP_LOGW("ble_interop", "Received unsupported GATT access operation: %d",
             ctxt->op);
    break;
  }

  return 0;
}

void ble_interop::notify_mesh_status(struct ble_npl_event *ev) {
  if (!ble_interop::instance()._is_connected) {
    ESP_LOGW("ble_interop",
             "Cannot notify mesh status, no active BLE connection");
    return;
  }

  ESP_LOGI("ble_interop", "Broadcasting mesh status notification");

  uint8_t buffer[mesh_interop_MeshInteropState_size];
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));

  if (write_mesh_interop_dto(&stream) != ESP_OK) {
    ESP_LOGE("ble_interop", "Failed to encode mesh _state for notification");
    return;
  }

  uint16_t val_handle;

  ble_gatts_find_chr(&BLE_INTEROP_SERVICE_UUID.u,
                     &BLE_INTEROP_MESH_STATUS_CHARACTERISTIC_UUID.u, nullptr,
                     &val_handle);

  int rc = ble_gattc_notify_custom(
      ble_interop::instance()._active_conn_handle, val_handle,
      ble_hs_mbuf_from_flat(buffer, stream.bytes_written));
  if (rc != 0) {
    ESP_LOGE("ble_interop", "Failed to send mesh status notification: %d", rc);
  } else {
    ESP_LOGI("ble_interop", "Mesh status notification sent successfully");
  }
}
