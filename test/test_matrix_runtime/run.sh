#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
test_binary="$(mktemp "${TMPDIR:-/tmp}/lumadrive-matrix.XXXXXX")"
trap 'rm -f -- "$test_binary"' EXIT
nanopb_dir=.pio/libdeps/esp32dev/Nanopb
runtime_flags=()
if [[ "${MATRIX_SANITIZE:-0}" == 1 ]]; then
  runtime_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic "${runtime_flags[@]}" \
  -I test/test_matrix_runtime/stubs -I test/test_animation_trigger_table/stubs \
  -I src -I generated -I "$nanopb_dir" \
  test/test_matrix_runtime/test_service.cpp src/matrix_runtime/matrix_core.cpp \
  src/color/color.cpp generated/mesh.pb.c \
  "$nanopb_dir/pb_common.c" "$nanopb_dir/pb_decode.c" "$nanopb_dir/pb_encode.c" \
  -o "$test_binary"
"$test_binary"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror "${runtime_flags[@]}" \
  -I test/test_matrix_runtime/stubs -I test/test_animation_trigger_table/stubs -I src \
  test/test_matrix_runtime/test_clock.cpp src/master_clock_manager/master_clock_manager.cpp \
  -o "$test_binary"
"$test_binary"
