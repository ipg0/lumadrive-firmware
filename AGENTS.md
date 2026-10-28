# Repository Guidelines

## Project Structure & Module Organization

This repository contains ESP32 lighting firmware built with PlatformIO and ESP-IDF. `src/main.cpp` provides `app_main()` and initializes services and connected lighting drivers. `src/connected_lighting_drivers/` owns board wiring, persistent driver instances, and initialization. Other feature directories under `src/` separate BLE communication (`ble_interop/`), mesh networking (`mesh_interop/`), clock synchronization (`master_clock_manager/`), configuration, color utilities, lighting drivers, and animation triggers. Keep concrete driver and trigger implementations in their existing `impl/` directories.

`include/` and `lib/` are reserved for shared headers and private libraries; both currently contain placeholders. `test/` contains only a PlatformIO README. `generated/` holds ignored Nanopb output. Protocol definitions live in the sibling `../proto/` repository.

## Build, Test, and Development Commands

Run these commands from the repository root with PlatformIO Core installed:

- `pio run -e esp32dev`: build firmware for the configured ESP32 board.
- `pio run -e esp32dev -t upload`: build and flash a connected board; add `--upload-port <port>` when needed.
- `pio device monitor -b 115200`: inspect runtime logs.
- `pio run -e esp32dev -t clean`: remove build outputs.
- `pio test -e esp32dev`: run tests once suites are added.

Before building, ensure `generated/mesh.pb.c` and its header exist. With `protoc` and its Nanopb and Dart plugins on `PATH`, run `bash ../proto/generate.sh` to regenerate bindings for both projects. The script creates the output directories and writes firmware bindings to `generated/`, matching `src/CMakeLists.txt`.

## Coding Style & Naming Conventions

Follow existing C++ style: two-space indentation, opening braces on the declaration line, `.hpp` headers with `#pragma once`, and `.cpp` implementations. Use `snake_case` for files, classes, and functions; private members commonly start with `_`. Follow nearby constant conventions, including uppercase enum values and `TAG` logging identifiers. No repository formatter or linter configuration is present; avoid unrelated reformatting.

## Testing Guidelines

No test suites or coverage thresholds are established. Add future PlatformIO suites under `test/test_<feature>/` using descriptive test names. Build changed firmware and validate affected behavior on hardware, especially pairing, reconnects, clock synchronization, and LED output. Record board wiring and relevant serial logs.

## Commit & Pull Request Guidelines

History uses short, imperative subjects with prefixes such as `feat:` and `refactor:`. Keep commits focused. PRs should describe the behavior change, link relevant issues, and report build results and hardware checks. Include logs or photos when useful for communication or lighting changes. Exclude `.pio/`, `managed_components/`, and generated protocol outputs from commits.

## Agent Instructions

Use `apply_patch` for file edits. Prefer connected Codex extension tools for file reads when available. Preserve unrelated working-tree changes.
