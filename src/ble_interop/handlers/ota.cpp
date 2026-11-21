#include "../ble_interop.hpp"
#include "ota_update/ota_update.hpp"

int ble_interop::handle_ota_upload(uint16_t, uint16_t, ble_gatt_access_ctxt *ctx, void *) {
  if (ctx->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  uint8_t bytes[512];
  const size_t size = OS_MBUF_PKTLEN(ctx->om);
  if (!size || size > sizeof(bytes) || os_mbuf_copydata(ctx->om, 0, size, bytes))
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  return ota_update::submit(bytes, size) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

int ble_interop::handle_ota_status(uint16_t, uint16_t, ble_gatt_access_ctxt *ctx, void *) {
  if (ctx->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
  uint8_t bytes[16]; ota_update::status(bytes);
  return os_mbuf_append(ctx->om, bytes, sizeof(bytes)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}
