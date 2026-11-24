#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
test_binary="$(mktemp "${TMPDIR:-/tmp}/lumadrive-ota.XXXXXX")"
trap 'rm -f -- "$test_binary"' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I test/test_mesh_ota/stubs -I test/test_matrix_runtime/stubs \
  -I test/test_animation_trigger_table/stubs -I src \
  test/test_mesh_ota/test_main.cpp src/ota_update/mesh_ota.cpp -o "$test_binary"
"$test_binary"
