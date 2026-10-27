#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
python3 -m venv .venv-build
PYTHON="$PWD/.venv-build/bin/python"
if ! "$PYTHON" -m platformio --version >/dev/null 2>&1; then
    "$PYTHON" -m pip install 'platformio>=6.1.19,<7'
fi
if [[ ! -f src/secrets.h ]]; then cp src/secrets.example.h src/secrets.h; fi
"$PYTHON" tools/make_web.py
"$PYTHON" -m platformio run -e esp32s3-eth
mkdir -p firmware
cp .pio/build/esp32s3-eth/bootloader.bin firmware/bootloader.bin
cp .pio/build/esp32s3-eth/partitions.bin firmware/partition-table.bin
cp .pio/build/esp32s3-eth/firmware.bin firmware/firmware.bin
(cd firmware && sha256sum bootloader.bin partition-table.bin firmware.bin > SHA256SUMS)
echo 'Build complete. Flash with: bash flash_ubuntu.sh'
