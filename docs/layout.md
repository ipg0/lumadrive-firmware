# Car layout

`MatrixConfig.layout` is the single authoritative physical arrangement of the
car's outputs. It is part of the persisted configuration, shared by all nodes.
Every animation renders on this entire linear canvas. Entries contain effect,
trigger, condition, and priority references; they have no target or layout fields.
An effect may leave canvas pixels transparent, but cannot select a separate surface.

Each layout element identifies a node's ESP-NOW `node_mac`, firmware `driver` slot,
`source_index`, and boundary indices `from` and `to`. A strip uses source 0 and
its configured length. A 595 exposes each RGB LED as a separate one-pixel source
(indices 0 and 1), independently placeable anywhere on the canvas.
Boundaries are half-open, so swapping them
reverses the same covered region:

| Placement | Physical pixel 0 maps to | Remaining mapping |
| --- | --- | --- |
| `0 → 60` | canvas pixel 0 | 1, 2, …, 59 |
| `120 → 60` | canvas pixel 119 | 118, 117, …, 60 |
| `10 → 12` | canvas pixel 10 | 11 |
| `12 → 10` | canvas pixel 11 | 10 |

The span `abs(to - from)` sets a strip's LED count, up to the device-reported
`max_pixel_count` (currently 2,048 for WS2812), or exactly one pixel for an
individual RGB LED. Increasing the length reserves larger render/transmit buffers
on its owner without reallocating RMT channels. Allocation failure rejects preparation;
the previous matrix continues. There is no scaling or configurable wiring.
Endpoints must differ and lie in 0–65,536. Node/driver/source tuples are unique.
Up to six physical drivers per node and 84 placed sources are supported, enough
for seven nodes with six two-LED modules each. Mesh pairing still permits 17 nodes
including the master, sharing the same total source limit.
Nodes must be paired; each owner validates its physical output lengths before
accepting a revision. Before persistence the master validates its own lengths and
remote lengths already reported by those nodes.

Overlaps and gaps are allowed. A strip at `0 → 60` and a 595 LED at `10 → 11`
both display canvas pixel 10. The module's other LED may be at `70 → 71`.
Overlap copies the same composed samples
to separate physical outputs. The canvas extends from zero to the greatest
endpoint; gaps remain part of that coordinate space.

```dart
final config = MatrixConfig(
  schemaVersion: 2,
  layout: [
    MatrixLayout(nodeMac: stripNodeMac, driver: MatrixDriver.WS2812,
      from: 0, to: 60),
    MatrixLayout(nodeMac: otherStripNodeMac, driver: MatrixDriver.WS2812,
      from: 120, to: 60),
    MatrixLayout(nodeMac: indicatorNodeMac, driver: MatrixDriver.HC595,
      sourceIndex: 0, from: 10, to: 11),
    MatrixLayout(nodeMac: indicatorNodeMac, driver: MatrixDriver.HC595,
      sourceIndex: 1, from: 70, to: 71),
  ],
  // effects, sources, conditions, and entries describe animations on this canvas.
);
```

Keyframe positions, widths, and wrapping use global canvas coordinates. Default
gradient wavelength spans the full canvas. Every node receives the same canvas
size and only its own physical mappings; players sample just their visible
pixels, with no full-canvas buffer. Each physical driver is cleared/presented
once per frame after priority and alpha composition.

On a fresh master, the initial configuration places every output at
`0 → source_pixel_count`: strips and individual RGB LEDs overlap, with no device offsets.
Its canvas is as long as the largest driver, and a repeating rainbow runs on it.
Discovered slave outputs are added with the same zero-based placement while the
configuration remains factory-owned. Saving any user configuration, including a
layout-only edit, ends automatic placement.

Layout and animations activate together at the revision's shared timestamp and
are restored together after reboot/reconnect. Only the current configuration
format is supported. Development configurations may be discarded after format
changes; there are no legacy parsers or migration paths. The schema version is
a format check, not a selection between layout models.

## Configuring in the control app

Open **Car layout** from the connected device home screen. **Simple** shows a
top-down car with four doors, four footwells, dash left/right, and center console
left/right. Each door also offers **Handle** and **Pocket** locations. Select a
location and assign an output, then set its name, LED count, and direction.
Choosing an output in **Assign a light to this location** immediately runs its
Identify preview. The selected-output card also offers **Assign to [location]**
for the currently highlighted segment, including a door's handle or pocket.
Segment selection stays shared with the graphic while scrolling; assignment
updates only the draft until Apply is pressed.
Assigned outputs pack continuously from the rear left door, up the left side,
across the dash, and down the right side to the rear right door. The persisted
`car_position` field records the selected location. Outputs at the same location
retain their registration order. Length edits and removals in Simple repack this line.

Switch to **Advanced** to drag bars along the ruler or enter exact indices,
including gaps and overlaps. Manual position edits clear that output's car
location. **View zoom** changes the visual scale only. Switching modes never
rewrites positions; existing unassigned placements stay intact until assigned.
Both views edit the same draft and produce the same authoritative layout.
Outputs are grouped by node and report their actual firmware driver type and
availability. Firmware wiring and the initial default length remain static C++.
Configured lengths and locations restore after reboot and transfer to slaves.
Individual RGB LEDs appear as point markers with independent names and positions;
they have no reversal control. Identifying one LED leaves its sibling playing.
Overlapping bars intentionally share canvas colors. Zoom controls help place
short modules beside longer strips.

**Identify** fills green from the configured start over two seconds, then holds
the complete strip lit for one second, with the final pixel blue.
It previews the draft LED count and direction without
saving; unconfigured trailing pixels stay off during the test. An individual
RGB LED pulses green instead. Playback continues advancing underneath and
resumes at its current phase. Identification also works before placing an output
by first adding it to the local draft; Apply is not required.

Drafts and last-known inventory are stored on the phone per controller. Offline
outputs retain their placements; an unknown output cannot be placed until its
actual length arrives. **Apply** sends a revision-checked layout update. Firmware
merges only layout and names into the saved matrix, preserves all animations and
triggers, validates and persists it, then deploys the new revision. If another
client changes the configuration, the editor keeps the draft and asks you to load
the saved layout before applying again. A first layout can be saved with no
animations configured.

Background inventory refreshes keep Apply and Identify available. Pressing either
queues it behind an in-progress read; only the requested action shows a busy state.
