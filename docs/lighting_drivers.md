# Connected lighting drivers

Board wiring lives in `src/connected_lighting_drivers/connected_lighting_drivers.cpp`.
Driver ID 1 is the 60-pixel WS2812 strip on GPIO19. Driver ID 2 remains `HC595` in
the protocol and exposes one RGB pixel through the TPIC6B595N; its layout indices
and animation behavior are unchanged.

## Static registry

`DRIVERS` in the C++ file registers up to six physical instances using stable
slot IDs 1–6. Driver type is inferred from the instance; IDs do not encode type.
Add the configuration, static instance, and registry entry in that same file:

```cpp
constexpr ws2812_driver_config REAR_CFG = {.data_pin = GPIO_NUM_21, .led_count = 30};
ws2812_lighting_driver rear_strip(REAR_CFG);
// In DRIVERS, alongside the current entries:
register_driver(3, rear_strip),
```

Slots may be sparse. Duplicate IDs or overlapping output pins are rejected.
`uses_gpio()` derives reserved pins from every registered instance. Keep wiring
specific to each board's build; the master uses each node's actual inventory.

One 595 driver configured with `led_count = 2` exposes two independent layout
sources, each length one, addressed by `(node_mac, driver, source_index)`.
There is no requirement that the two LEDs be adjacent in the car layout.
Configure both RGB bit mappings in `leds`; no COM output is used.

## TPIC6B595N

SER IN connects to GPIO23, SRCK to GPIO18, and RCK to GPIO5. RGB pixel 0 uses
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
