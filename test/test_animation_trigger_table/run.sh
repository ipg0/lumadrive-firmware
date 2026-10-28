#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
test_binary="$(mktemp "${TMPDIR:-/tmp}/lumadrive-trigger-matrix.XXXXXX")"
trap 'rm -f -- "$test_binary"' EXIT

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I test/test_animation_trigger_table/stubs -I src \
  test/test_animation_trigger_table/test_main.cpp \
  src/animation_trigger_table/animation_trigger_table.cpp \
  src/color/color.cpp -o "$test_binary"
"$test_binary"
