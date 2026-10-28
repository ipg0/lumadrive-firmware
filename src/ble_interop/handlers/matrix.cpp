#include "../ble_interop.hpp"
#include "matrix_runtime/matrix_service.hpp"
#include "matrix_runtime/matrix_core.hpp"
#include "nimble/nimble_port.h"

uint16_t ble_interop::_matrix_status_handle = 0;
namespace { ble_npl_event status_event; }

void ble_interop::init_matrix_notifications() {
  ble_npl_event_init(&status_event, [](ble_npl_event *) {
    // The notification is an invalidation signal, deliberately one byte so it
    // works at MTU 23. Clients read the complete status through ATT long reads.
    auto &self = ble_interop::instance();
    std::lock_guard<std::mutex> lock(self._state_mutex);
    if (!self._is_connected) return;
    const uint8_t changed = 1;
    auto *om = ble_hs_mbuf_from_flat(&changed, 1);
    if (om) ble_gatts_notify_custom(self._active_conn_handle, _matrix_status_handle, om);
  }, nullptr);
  matrix_service::set_status_callback([]() {
    if (!ble_npl_event_is_queued(&status_event))
      ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &status_event);
  });
}

int ble_interop::handle_matrix_upload(uint16_t connection, uint16_t,
                                      ble_gatt_access_ctxt *ctx, void *) {
  if (ctx->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  uint16_t length = OS_MBUF_PKTLEN(ctx->om);
  uint8_t bytes[512];
  if (!length || length > sizeof(bytes) || os_mbuf_copydata(ctx->om, 0, length, bytes))
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  return matrix_service::upload(connection, bytes, length) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
int ble_interop::handle_matrix_status(uint16_t, uint16_t,
                                      ble_gatt_access_ctxt *ctx, void *) {
  if (ctx->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
  uint8_t bytes[512];
  const auto size = matrix_service::read_status(bytes, sizeof(bytes));
  if (!size) return BLE_ATT_ERR_INSUFFICIENT_RES;
  return os_mbuf_append(ctx->om, bytes, size) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
