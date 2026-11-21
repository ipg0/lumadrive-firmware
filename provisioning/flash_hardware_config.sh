#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 2 ]]; then
  printf 'Usage: bash provisioning/flash_hardware_config.sh CONFIG.hpp SERIAL_PORT\n' >&2
  exit 2
fi
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
image_path="$(mktemp "${TMPDIR:-/tmp}/lumadrive-hardware.XXXXXX")"
trap 'rm -f -- "$image_path"' EXIT
bash "$project_dir/provisioning/build_hardware_image.sh" "$1" "$image_path"
esptool_path="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/tool-esptoolpy/esptool.py"
if [[ ! -f "$esptool_path" ]]; then
  printf 'PlatformIO esptool not found: %s. Build firmware first or set PLATFORMIO_CORE_DIR.\n' "$esptool_path" >&2
  exit 1
fi
printf -v flash_command '%q ' python "$esptool_path" --chip esp32 --port "$2" \
  write_flash --flash_size 2MB 0x12000 "$image_path"
pio pkg exec -p tool-esptoolpy -c "$flash_command"
