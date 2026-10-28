# Trigger matrix

This describes the standalone local table API. The firmware's BLE-configured,
master-controlled runtime uses the separate evaluator/player service described
in [Distributed matrix protocol](matrix_protocol.md).

The table supports up to 32 entries. Each binds an effect descriptor, source,
condition, target driver, and priority. Initialize connected drivers first, then
register entries with `add_entry()`. The table creates one reusable effect
instance per registration; `tick()` does not allocate instances.

All configuration objects and drivers are borrowed and must outlive their
registrations. Use one task for registration, removal, and ticking. Each frame
clears the participating drivers to black, renders active effects from lowest to
highest priority, and calls `show()` once per driver. Equal priorities render in
registration order. Effects draw into the frame buffer using driver alpha
blending and must not clear or present the frame themselves.

Default-state entries remain active while their condition and source are true.
They restart after completion if still enabled. GPIO entries start on a matching
MAC, pin, and edge; they continue without further events until finished. Another
accepted edge restarts the existing instance. Conditions gate new GPIO events,
without cancelling an already running effect.

Static-color effects fill the target driver. A duration of zero holds the color;
a nonzero duration expires after that many milliseconds. Higher-priority
transparent colors reveal the layers below.

```cpp
#include "animation_trigger_table/animation_trigger_table.hpp"
#include "animation_trigger_table/animation_effect/impl/static_color_animation_effect.hpp"
#include "animation_trigger_table/trigger_condition/impl/always_trigger_condition.hpp"
#include "animation_trigger_table/trigger_source/impl/default_state_trigger_source.hpp"
#include "connected_lighting_drivers/connected_lighting_drivers.hpp"
#include "master_clock_manager/master_clock_manager.hpp"

// Keep these objects alive for the duration of the animation task.
trigger_source<trigger_source_type::DEFAULT_STATE> source;
trigger_condition<trigger_condition_type::ALWAYS> condition;
animation_effect_descriptor<animation_effect_type::STATIC_COLOR> effect(
    alpha_color(color(255, 0, 0), 128));
animation_trigger_table table;
ESP_ERROR_CHECK(table.add_entry(
    {&effect, &source, &condition, &connected_lighting_drivers::ws2812(), 10}));

// Once per frame: supply shared-clock milliseconds and only this frame's edges.
trigger_context context;
context.now_ms = static_cast<uint32_t>(
    master_clock_manager::instance().get_master_time() / 1000);
ESP_ERROR_CHECK(table.tick(context));
```

Set `gpio_edges` and `gpio_edges_count` to a borrowed event array for that frame.
Context collection, mesh propagation, task scheduling, and application-specific
trigger bindings remain the caller's responsibility.

`remove_entry(index)` uses registration indices and recomposes surviving layers;
later indices shift down. `clear()` blanks outputs and releases instances. Check
returned ESP errors; failed output operations retain registrations for retry.
Destruction releases instances without accessing hardware; call `clear()` first
when outputs should be blanked.

Run hardware-independent tests with:

```sh
bash test/test_animation_trigger_table/run.sh
```

These host tests use a C++20 compiler and minimal ESP header stubs; they do not
require a board or the PlatformIO test runner.
