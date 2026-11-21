#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
test_binary="$(mktemp "${TMPDIR:-/tmp}/lumadrive-hardware-config.XXXXXX")"
trap 'rm -f -- "$test_binary"' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic -I src \
  test/test_hardware_config/test_main.cpp -o "$test_binary"
"$test_binary"
