#!/usr/bin/env bash
# Flash the three verified build images without erasing saved NVS settings.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if [[ ! -f firmware/firmware.bin || ! -f firmware/bootloader.bin || ! -f firmware/partition-table.bin ]]; then
    echo "Missing firmware images. Run bash build_ubuntu.sh first."
    exit 1
fi
if ! python3 -m venv .venv-flash 2>/dev/null; then
    echo 'Install the Python venv package, then run this script again:'
    echo 'sudo apt install python3-venv'
    exit 1
fi
PYTHON="$PWD/.venv-flash/bin/python"
if ! "$PYTHON" -m esptool version >/dev/null 2>&1; then
    "$PYTHON" -m pip install 'esptool>=5,<6'
fi
PORT="${1:-}"
if [[ -z "$PORT" ]]; then
    PORT="$("$PYTHON" - <<'PY'
import sys
from serial.tools import list_ports
ports = [p for p in list_ports.comports() if p.vid == 0x303a]
if len(ports) != 1:
    print('Connect the ESP32 by USB-C. If needed, specify the port:', file=sys.stderr)
    print('bash flash_ubuntu.sh /dev/ttyACM0', file=sys.stderr)
    for p in list_ports.comports(): print(f'  {p.device}: {p.description}', file=sys.stderr)
    sys.exit(1)
print(ports[0].device)
PY
)"
fi
if [[ ! -r "$PORT" || ! -w "$PORT" ]]; then
    echo "Cannot access $PORT. Add your user to dialout, then log out and back in:"
    echo 'sudo usermod -aG dialout "$USER"'
    exit 1
fi
if [[ -f firmware/SHA256SUMS ]]; then
    (cd firmware && sha256sum --check SHA256SUMS)
fi
echo "Flashing Net Ghost v2.0 on $PORT. Saved NVS settings are preserved."
"$PYTHON" -m esptool --chip esp32s3 --port "$PORT" --baud 460800 \
    write-flash --flash-size 16MB \
    0x0 firmware/bootloader.bin \
    0x8000 firmware/partition-table.bin \
    0x10000 firmware/firmware.bin
echo 'Done. Press RESET if the screen does not restart automatically.'
