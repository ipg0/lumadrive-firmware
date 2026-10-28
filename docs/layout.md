# Car layout

`MatrixConfig.layout` is the single authoritative physical arrangement of the
car's outputs. It is part of the persisted configuration, shared by all nodes.
Every animation renders on this entire linear canvas. Entries contain effect,
trigger, condition, and priority references; they have no target or layout fields.
An effect may leave canvas pixels transparent, but cannot select a separate surface.

Each layout element identifies a node's ESP-NOW `node_mac`, firmware `driver` ID,
and boundary indices `from` and `to`. Boundaries are half-open, so swapping them
reverses the same covered region:

| Placement | Physical pixel 0 maps to | Remaining mapping |
| --- | --- | --- |
| `0 → 60` | canvas pixel 0 | 1, 2, …, 59 |
| `120 → 60` | canvas pixel 119 | 118, 117, …, 60 |
| `10 → 12` | canvas pixel 10 | 11 |
| `12 → 10` | canvas pixel 11 | 10 |

The span `abs(to - from)` must match the physical driver's pixel count. There
is no scaling or configurable wiring. Endpoints must differ and lie in
0–65,536. Node/driver pairs are unique, with at most 34 elements on 17 nodes.
Nodes must be paired; each owner validates its physical output lengths before
accepting a revision. The master additionally validates its own output lengths
before persistence.

Overlaps and gaps are allowed. A strip at `0 → 60` and a 595 module at `12 → 10`
both display canvas pixels 10 and 11. Overlap copies the same composed samples
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
      from: 12, to: 10),
  ],
  // effects, sources, conditions, and entries describe animations on this canvas.
);
```

Keyframe positions, widths, and wrapping use global canvas coordinates. Default
gradient wavelength spans the full canvas. Every node receives the same canvas
size and only its own physical mappings; players sample just their visible
pixels, with no full-canvas buffer. Each physical driver is cleared/presented
once per frame after priority and alpha composition.

Layout and animations activate together at the revision's shared timestamp and
are restored together after reboot/reconnect. Only the current configuration
format is supported. Development configurations may be discarded after format
changes; there are no legacy parsers or migration paths. The schema version is
a format check, not a selection between layout models.
