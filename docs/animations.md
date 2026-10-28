# Animation effects

Effects render into driver buffers and obey matrix priority/alpha blending.
The master schedules starts; all nodes derive phase from elapsed shared-clock
time. Late commands and recovery snapshots seek directly to the same phase.
`duration_ms` is a lifetime independent of an animation's cycle: zero runs until
stopped; a nonzero value expires at that elapsed time. Retriggers restart phase.
All animations use the one [Car layout](layout.md) from the configuration.
Entries have no per-output targeting or alternative animation surfaces.

## Spatial keyframes and motion

`KEYFRAMES` contains up to 16 frames across independent spatial tracks. Each
frame identifies its track by `track_id` (zero is the default track) and specifies
RGBA, `position`, and `width`. Position/width use pixel units and support fractions.
Position is the track's left edge; pixel 0 occupies `[0, 1)`. Width defaults to
one pixel; an explicit zero collapses the track. Interpolation moves and resizes
the shape while changing its color. Two frames can move a band across a whole
strip; no per-LED timing configuration is needed.

Each track must have at least two frames, start at `time_ms: 0`, and have strictly
increasing times in list order. Different tracks may be interleaved in the list.
Tracks can overlap, compositing in order of first appearance in the frame list.
Uncovered pixels retain the layers below. Partial pixel coverage scales alpha,
so fractional motion blends across adjacent LEDs. Without wrapping, shapes clip
at the driver bounds; negative positions allow a shape to enter from off-strip.
`wrap: true` flows through the end of the global canvas back to pixel zero.
Positions may span multiple canvas lengths to express multiple revolutions.
Wrapped widths at least as wide as the canvas cover every pixel once. Positions must be finite and within
±1,000,000 pixels; widths must be finite and within 0–65,536 pixels.

Set `all_pixels: true` explicitly for a whole-canvas color track. It ignores
position/width. `wrap` and `all_pixels` must stay constant within a track.
RGBA channels are 0–255; alpha defaults to transparent. Default geometry targets
pixel 0 only. Keeping geometry constant is also supported for fixed regions.
Encoded per-node definitions must also fit the 16 KiB transfer limit; excessive
expansion from reused effects is rejected before replacing the current matrix.

Interpolation belongs to the segment **from** that frame to the next:

| Value | Behavior |
| --- | --- |
| `LINEAR` (default) | Interpolate RGBA, position, and width linearly. |
| `STEP` | Hold the current frame until the next timestamp. |
| `SMOOTH` | Use smoothstep easing with zero slope at both endpoints. |

With `loop: false`, tracks hold their last pose/color. With `loop: true`, all tracks
wrap together at the greatest keyframe timestamp; shorter tracks hold their last
poses/colors until that boundary. Match endpoints for seamless loops; wrapped
positions separated by exactly one canvas length also match spatially.

The Dart upload API accepts generated messages directly:

```dart
final effect = MatrixEffect(
  id: 1, kind: MatrixEffect_Kind.KEYFRAMES, loop: true,
  keyframes: [
    // A six-pixel red band travels around a 60-pixel strip every two seconds.
    MatrixKeyframe(timeMs: 0, trackId: 1, position: 0, width: 6,
      wrap: true, red: 255, alpha: 255),
    MatrixKeyframe(timeMs: 2000, trackId: 1, position: 60, width: 6,
      wrap: true, red: 255, alpha: 255),
  ],
);
```

Add more tracks for multiple moving shapes. Add a middle frame with a different
position/width/color for bouncing, expanding pulses, or acceleration using
`SMOOTH` easing. For a continuous color field across the strip, use the parametric
flowing gradient below instead of configuring LED colors individually.

## Parametric effects

The primary color uses `MatrixEffect.red/green/blue/alpha`. A zero `period_ms`
defaults to 1000 ms; explicit periods must be at least 2 ms. Rendering runs at
10 ms intervals, so use slower cycles for visible transitions.

| Type | Parameters and behavior |
| --- | --- |
| `STATIC_COLOR` | Hold the primary RGBA color. |
| `BLINK` | Primary color on first, then transparent. `duty_percent` is the on fraction, 1–100; zero defaults to 50. |
| `BREATHE` | Smoothly vary primary alpha from zero to its configured value and back, peaking halfway through `period_ms`. |
| `FLOWING_GRADIENT` | Move a repeating primary → `secondary_color` → primary gradient along canvas indices. Requires secondary RGBA. `wavelength_pixels: 0` fits one cycle to the full canvas; a positive value sets its spatial length. `reverse: true` flows toward decreasing indices. |

These parametric types cover the whole canvas. Blink/breathe off phases reveal
lower layers. Gradient interpolation includes alpha and uses logical RGB pixels
on both WS2812 and HC595 drivers. All participating firmware must understand the
effect kinds and current schema version used in the configuration. Earlier
development configuration formats are unsupported.

## Native C++ descriptors

Include `keyframe_animation_effect.hpp` or `periodic_animation_effect.hpp` from
`animation_trigger_table/animation_effect/impl/`. Keyframe descriptors copy their
frame arrays. For example:

```cpp
animation_keyframe frames[] = {
  // time, RGBA, interpolation, track ID, position, width, wrap
  {0, alpha_color(color(255, 0, 0)), animation_interpolation::LINEAR, 1, 0, 6, true},
  {2000, alpha_color(color(255, 0, 0)), animation_interpolation::LINEAR, 1, 60, 6, true},
};
animation_effect_descriptor<animation_effect_type::KEYFRAMES> effect(frames, 2, true);

periodic_animation_options options;
options.primary = alpha_color(color(0, 100, 255));
options.period_ms = 2000;
animation_effect_descriptor<animation_effect_type::BREATHE> breathe(options);
```

Register descriptors using the existing table API. Invalid descriptors return
`nullptr` from `create_instance()`. Distributed players validate the same rules
when preparing received definitions. Rendering does not allocate or call `show()`.
