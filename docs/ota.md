# Firmware updates

Flash a firmware build containing the OTA receiver over USB first. Connect the app to the master, select **Update firmware**, and choose the ESP32 application `firmware.bin` (not a merged flash or hardware configuration image). The app sends the file to the inactive OTA slot, waits for validation, and requests a restart. The existing NVS and hardware configuration partitions are not rewritten. The slave nodes are not updated by this flow.

The current build does not enable ESP-IDF rollback. Image validation detects malformed firmware but cannot guarantee that a valid image will start successfully; USB recovery remains necessary if it does not boot. Changes to the partition table, including the hardware partition rename, require a USB firmware flash.

The encrypted BLE characteristics `294b8b31-57d2-bf85-dc41-5c612c9d1539` (write) and `294b8b32-57d2-bf85-dc41-5c612c9d1539` (read) use little-endian fixed-width values. Every command starts with a one-byte operation and a four-byte nonzero transfer ID. Begin (1) adds a four-byte image length and IEEE CRC32; chunk (2) adds a four-byte offset and data; validate (3), abort (4), select boot partition (5), and restart (6) have no additional fields. Send one operation at a time and poll status for acknowledgement. Each frame must fit the negotiated ATT MTU minus three bytes and the 512-byte characteristic limit.

Status is 16 bytes: stage, error, two reserved bytes, transfer ID, accepted offset, and total length. Stages are idle (0), receiving (1), validated (2), failed (3), and selected (4). A disconnected or inactive upload is aborted; selection occurs only after the full length, CRC, and ESP-IDF image validation succeed. A selected image must be rebooted before starting another upload.
