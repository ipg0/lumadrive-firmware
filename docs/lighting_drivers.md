# Connected lighting drivers

Each node's role and wiring are provisioned in a dedicated hardware configuration
partition. Firmware and the app contain no default pin assignments. Copy
`provisioning/example_hardware_config.hpp` to a file for each node and edit its
`HARDWARE_CONFIGURATION`. Set `config.role` to `NODE_MASTER` or `NODE_SLAVE`.
Keep driver IDs 1–6
stable because saved layouts reference each node's MAC, driver ID and source index.

## USB provisioning

The hardware partition has been renamed and requires a **one-time USB flash**
of the bootloader, partition table and firmware on every node. Existing driver-only
records do not specify a role and must be replaced with a hardware configuration.
NVS and PHY retain
their offsets; do not use `erase_flash` if pairing and saved configuration must be
preserved. Back up important data before migration. For each node, with the correct
header and serial port:

```sh
pio run -e esp32dev -t upload --upload-port /dev/ttyUSB0
LUMADRIVE_HARDWARE_CONFIG=/path/to/my_node.hpp pio run -e esp32dev -t provision-hardware --upload-port /dev/ttyUSB0
```

The second command validates the header, generates an image, and writes only the
`hardwarecfg` partition at `0x12000`. Repeat it to change role or wiring; the firmware does
not write this partition. Never include a configuration image in a firmware/OTA
package. Re-provisioning is not power-fail atomic: if interrupted, rerun the USB
command. The header can instead be specified with `custom_hardware_config` in
`platformio.ini`; `LUMADRIVE_HARDWARE_CONFIG` takes precedence for one run. A port
is required, either via `--upload-port` or the `upload_port` setting. The target
is independent of `upload` and never runs during a normal firmware build. To
generate an image without flashing:

```sh
bash provisioning/build_hardware_image.sh /path/to/my_node.hpp /tmp/my_node.bin
```

The versioned record is checksummed and validated on the host and device. Invalid
or missing records leave lighting and networking disabled and log the error; re-provision
over USB. A configuration with no drivers or inputs is allowed when its role is set.
Output pins must be valid ESP32 output GPIOs; flash, serial,
and discovery GPIO16 are reserved. Duplicate IDs, overlapping pins, invalid RGB
bit mappings, and conflicting shared BCM timer periods are rejected. `uses_gpio()`
derives reserved pins from the loaded registry; the master uses each node's actual
inventory. The app cannot edit wiring.

Configure trigger inputs alongside drivers with `config.gpio_count` and
`config.gpios`, as shown in the example header. Set each pin's `pull` to
`PULL_NONE`, `PULL_UP`, or `PULL_DOWN`. The ESP32's GPIO34–39 are input-only
and have no internal pull resistors, so they require `PULL_NONE` and an
external bias where needed. Driver pins, duplicate inputs, flash/serial pins,
GPIO16 are rejected. GPIO4 is now available; check the board's boot-strapping requirements
before choosing an input or pull. Inputs are configured at startup; interrupt
handlers are installed when the matrix uses them. Re-provision each node over
USB to change the list. Existing matrix entries referring to removed
inputs need to be changed or re-provisioned before they can run.

Set `.inverted = true` for an active-low input. Its physical falling edge is
reported to the matrix as a logical rising edge, and its physical rising edge
as a logical falling edge. Inversion does not change the selected pull resistor.
It defaults to `false`.

One 595 driver configured with `led_count = 2` exposes two independent layout
sources, each length one, addressed by `(node_mac, driver, source_index)`.
There is no requirement that the two LEDs be adjacent in the car layout.
Configure both RGB bit mappings in `leds`; no COM output is used.

## TPIC6B595N

In the example wiring, SER IN connects to GPIO23, SRCK to GPIO18, and RCK to GPIO5. RGB pixel 0 uses
DRAIN0/1/2. DRAIN3–7 are spare and are always
shifted as zero. The LED common anodes connect directly to the positive LED supply
(+12 V for this board), not to shift-register outputs. Each cathode is switched by
its corresponding drain output. The chip provides low-side current sinks, so it
cannot directly drive common-cathode LEDs from their anodes.

`hc595_led_config` contains only RGB bit indices and `channels_active_low`.
Use `false` for TPIC6B595: a stored 1 enables the sink and lights the channel.
Use `true` only for output stages where a stored 0 lights a channel. COM polarity
does not determine data polarity. Initialization, clear, brightness zero, and
shutdown produce the correct off pattern. RGB mappings
must be unique bits 0–7, with one or two configured pixels and a nonzero base period.

All registered modules share one priority-3 GPTimer interrupt on the lighting core,
so six modules do not exhaust hardware timers. Their `bcm_base_period_us` values
must match; mismatches are rejected at initialization. The callback uses
IRAM-safe timer/GPIO calls so flash writes do not defer the pulse transitions.
The eight planes retain weights 1, 2, 4, …, 128; at the configured 10 µs base,
a cycle takes approximately 2.55 ms plus shifting/interrupt overhead (~400 Hz).
`set_pixel()`, `fill()`, and `clear()` edit a working buffer. `show()` publishes
all eight precomputed planes, which the interrupt adopts together at the next
cycle boundary. Intermediate rendering clears never reach the physical outputs.

The hardware must hold output enable `/G` low and `/SRCLR` high for normal use;
these pins are not controlled by firmware. The IC's logic supply is 4.5–5.5 V,
separate from the 12 V LED supply. Its specified high-level input threshold is
0.85 × VCC, so ESP32 3.3 V signals need suitable level shifting to meet that spec.
See the [TI datasheet, sections 5.3 and 7.3](https://www.ti.com/lit/ds/symlink/tpic6b595.pdf).

Run `bash test/test_shift_register_driver/run.sh` for serial-output regression
tests; add `DRIVER_SANITIZE=1` for address/undefined-behavior sanitizers. Physical
output voltage, current, and wiring still require checks on the actual board.

## WS2812 timing and radio coexistence

Initialization runs in a short task pinned to core 1 on the dual-core ESP32.
The RMT interrupt is allocated on that core, separate from the Wi-Fi and NimBLE
tasks on core 0. Matrix evaluation and playback also stay on core 1. Merely moving
rendering after allocating RMT on core 0 would leave its interrupt on the radio core.

The registry divides the ESP32's eight 64-symbol RMT blocks among configured
strips: 512 symbols for one, 256 each for two, 128 each for three/four, and 64 each
for five/six. Allocation occurs on core 1. No DMA support is assumed on ESP32.
Verify smooth output during BLE advertising and connections, as well as ESP-NOW
discovery; an idle-radio test alone does not exercise interrupt contention.

`ws2812_driver_config.led_count` is the initial/default length. The app may set a
different count, currently 1–2,048, through layout boundaries. Checked allocations
grow the RGB and GRB buffers together and preserve existing pixels on failure.
The RMT channel and its core affinity stay in place. The simple encoder generates
the same 0.3/0.9 µs data timings and 280 µs reset as the previous strip component.
Shorter configurations send black through the retained buffer tail. Longer strips
take longer to transmit, so the effective render rate may fall below 100 Hz.
Inventory reports the initial length and maximum separately; a preview allocation
does not change the persisted configuration or factory layout.
