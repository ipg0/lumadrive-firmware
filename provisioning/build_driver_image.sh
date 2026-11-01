#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 2 ]]; then
  printf 'Usage: bash provisioning/build_driver_image.sh CONFIG.hpp OUTPUT.bin\n' >&2
  exit 2
fi
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
config_path="$(realpath -- "$1")"
test_binary="$(mktemp "${TMPDIR:-/tmp}/lumadrive-provision.XXXXXX")"
trap 'rm -f -- "$test_binary"' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I "$project_dir/src" -I "$(dirname -- "$config_path")" \
  "-DDRIVER_CONFIG_HEADER=\"$(basename -- "$config_path")\"" \
  "$project_dir/provisioning/create_driver_image.cpp" -o "$test_binary"
"$test_binary" "$2"
