# ESP32 matrix memory test

This opt-in test measures real ESP32 heap usage after radio initialization. It
does not modify the saved matrix or replace the active runtime. Flashing the
test image and rebooting will interrupt playback. Use a provisioned controller.

```sh
pio run -e memory_test -t upload
pio device monitor -b 115200
```

About ten seconds after startup, look for `memory_test` log lines. The test
allocates the signal history for 32 signals in two revisions, stages a maximum
16 KiB saved record, and decodes the controller's current matrix. It then frees
the staging buffers and prepares two replacement players while the existing
runtime and signal history remain allocated. Each stage reports total free heap,
largest contiguous block, and the system's minimum free heap. `FAIL` identifies
the first allocation or preparation that failed; `PASS` means these specific
operations succeeded. Restore the normal firmware afterwards:

```sh
pio run -e esp32dev -t upload
```

The test does not connect a BLE client, simulate seven nodes, create a maximum
complexity matrix, or prove immunity to fragmentation and future allocations.
For meaningful coverage, repeat it with representative large saved matrices and
with BLE connected. Minimum free heap is the ESP-IDF low-water mark since boot,
not a measurement isolated to a particular test stage. C++ allocations within
player preparation can abort the test firmware if the device runs out of memory;
a reboot or crash is a test failure.
