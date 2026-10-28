# Repository Guidelines

## Project Structure & Module Organization

This repository contains ESP32 lighting firmware built with PlatformIO and ESP-IDF. `src/main.cpp` provides `app_main()` and initializes services and connected lighting drivers. `src/connected_lighting_drivers/` owns board wiring, persistent driver instances, and initialization. Other feature directories under `src/` separate BLE communication (`ble_interop/`), mesh networking (`mesh_interop/`), clock synchronization (`master_clock_manager/`), configuration, color utilities, lighting drivers, and animation triggers. Keep concrete driver and trigger implementations in their existing `impl/` directories.

`src/matrix_runtime/` contains the master evaluator, node player, transfer worker, and persistence. `include/` and `lib/` are reserved for shared headers and private libraries. Host tests live in `test/`; `generated/` holds ignored Nanopb output. Protocol definitions live in the sibling `../proto/` repository.

## Build, Test, and Development Commands

Run these commands from the repository root with PlatformIO Core installed:

- `pio run -e esp32dev`: build firmware for the configured ESP32 board.
- `pio run -e esp32dev -t upload`: build and flash a connected board; add `--upload-port <port>` when needed.
- `pio device monitor -b 115200`: inspect runtime logs.
- `pio run -e esp32dev -t clean`: remove build outputs.
- `bash test/test_animation_trigger_table/run.sh`: run trigger-matrix host tests with a C++20 compiler.
- `bash test/test_matrix_runtime/run.sh`: test distributed playback, transfers, persistence, and clock synchronization with host adapters.

Before building, ensure `generated/mesh.pb.c` and its header exist. With `protoc` and its Nanopb and Dart plugins on `PATH`, run `bash ../proto/generate.sh` to regenerate bindings for both projects. The script creates the output directories and writes firmware bindings to `generated/`, matching `src/CMakeLists.txt`.

The build uses size optimization (`-Os`) and a custom `partitions.csv`: one 1.5 MiB app slot within a 2 MiB flash layout, preserving NVS and PHY offsets. OTA is not configured. SoftAP, enterprise Wi-Fi, WPA3 connection features, and IPv6 are disabled; ESP-NOW and BLE security/bond storage remain enabled.

## Coding Style & Naming Conventions

Use two-space indentation, opening braces on the declaration line, `.hpp` headers with `#pragma once`, and `.cpp` implementations. Project-owned C++ classes, type aliases, functions, variables, and template parameters use lowercase `snake_case`. Constants, enum values, and macros use `UPPER_SNAKE_CASE`. Private members may start with `_`. Preserve identifiers defined by generated code, ESP-IDF, FreeRTOS, and other external APIs. No repository formatter or linter configuration is present; avoid unrelated reformatting.

## Testing Guidelines

Trigger-matrix host tests live in `test/test_animation_trigger_table/` and cover blending, lifecycle, GPIO matching, cleanup, and failures using driver fakes and ESP header stubs. No coverage threshold is established. Add future suites under `test/test_<feature>/` using descriptive names. Build changed firmware and validate affected behavior on hardware, especially pairing, reconnects, clock synchronization, and LED output. Record board wiring and relevant serial logs. See `docs/trigger_matrix.md` for the matrix API and lifetime rules.

## Commit & Pull Request Guidelines

History uses short, imperative subjects with prefixes such as `feat:` and `refactor:`. Keep commits focused. PRs should describe the behavior change, link relevant issues, and report build results and hardware checks. Include logs or photos when useful for communication or lighting changes. Exclude `.pio/`, `managed_components/`, and generated protocol outputs from commits.

## Agent Instructions

Use `apply_patch` for file edits. Prefer connected Codex extension tools for file reads when available. Preserve unrelated working-tree changes.
