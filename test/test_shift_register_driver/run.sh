#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
test_binary="$(mktemp "${TMPDIR:-/tmp}/lumadrive-shift-register.XXXXXX")"
trap 'rm -f -- "$test_binary"' EXIT
runtime_flags=()
if [[ "${DRIVER_SANITIZE:-0}" == 1 ]]; then
  runtime_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic "${runtime_flags[@]}" \
  -I test/test_shift_register_driver/stubs -I test/test_animation_trigger_table/stubs -I src \
  test/test_shift_register_driver/test_main.cpp src/color/color.cpp \
  src/lighting_driver/impl/bcm_74hc595_lighting_driver.cpp -o "$test_binary"
"$test_binary"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic "${runtime_flags[@]}" \
  -I test/test_shift_register_driver/stubs -I test/test_animation_trigger_table/stubs -I src \
  test/test_shift_register_driver/test_registry.cpp src/color/color.cpp \
  src/lighting_driver/impl/ws2812_lighting_driver.cpp \
  src/lighting_driver/impl/bcm_74hc595_lighting_driver.cpp -o "$test_binary"
"$test_binary"
