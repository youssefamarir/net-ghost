# v1.6 validation

Done here (no hardware attached):
- Firmware builds with ESP-IDF 5.5 with zero warnings; image 1.88 MB of the 4 MB app partition,
  static internal RAM 60 %. TCP window 65534, AMPDU RX/TX, NimBLE bond storage confirmed in sdkconfig.
- Every screen page rendered in the desktop LVGL simulator (connected and disconnected data),
  checked against the round glass. PIN overlay and toasts rendered.
- Swipe navigation simulated with a virtual touch panel: left/right across column tops, up/down
  through every column, left/right from lower pages jumps to the next column top, edges stop.
- `ble.html` tested in headless Chrome against a fake Bluetooth device that relays to a mock API:
  connect, retry while pairing, every tab polling, settings save, link drop and auto-reconnect.

Check on the hardware:
- Wi-Fi and Ethernet speed tests (Tools column) against v1.5's numbers.
- Phone pairing: PIN appears on the tester, phone accepts it, reconnect without PIN, Forget phones.
- Bluetooth GUI from GitHub Pages in Chrome (Android) and Bluefy (iPhone).
- PoE page with a switch that sends LLDP-MED or CDP power TLVs.

# v1.5 validation

## Completed

- **ESP32-S3 target build passed** using PlatformIO Espressif32 6.12.0,
  ESP-IDF 5.5.0, GCC 14.2.0 and LVGL 8.4.0. Full successful output is in
  `firmware/BUILD_LOG.txt`.
- Linked static RAM: **75,828 bytes**. Linked flash usage: **1,848,795 bytes**
  of the 4 MB application partition. These are linker figures, not measured
  runtime free heap. Wi-Fi, TLS and UI allocate additional heap at runtime.
- `firmware.bin` is **1,849,200 bytes**. esptool 5.6.0 image inspection validates
  its checksum/hash, ESP32-S3 target, 16 MB flash setting, application version
  `1.5.0` and ESP-IDF `5.5.0`.
- Partition table checked: NVS `0x9000..0xefff`, PHY initialization at `0xf000`,
  4 MB factory app at `0x10000`. The flash script writes separate bootloader,
  partition and application files; it does not write or erase the NVS range.
- **PoE decoder tests passed** with AddressSanitizer/UndefinedBehaviorSanitizer:
  short/malformed TLVs, role flags, unknown class/pairs, 802.3 power scaling,
  LLDP-MED power, CDP available-power offset, minimum CDP length and integer range.
- **Status JSON stress fixture passed**: 24 APs, 10 alerts and escaped text;
  7,448-byte valid JSON in a 16,384-byte buffer. This is a stress fixture,
  not an exhaustive proof for every possible state/string combination.
- **Browser GUI checks passed** at 390-pixel mobile width: all eight tabs,
  no page errors/horizontal document overflow, Wi-Fi-test action, settings-save
  request, large-text rendering and hiding disconnected/stale PoE data.
- **Native LVGL software render passed** for all 19 round-display pages at
  both Standard and Large text sizes. New detail pages use vertical scrolling;
  long status values scroll horizontally within their allocated space.
- Ubuntu shell scripts pass `bash -n`; esptool 5.6.0 supports the flash command's
  options and segmented address/file format.
- The original display driver, pin definitions, packet analyzer, battery
  monitor and main runtime source remain unchanged.

The preview/test fixture uses synthetic example networks and speed values.
Those values are not real measurements and are not compiled into the firmware.
Only the application source files listed in `src/CMakeLists.txt` are compiled.

## Still requires the physical tester

No board was attached during this update. After flashing:

1. Confirm the original LCD, touch, Ethernet, QR/hotspot and existing tools work.
2. Join upstream Wi-Fi in the GUI, then run **Wi-Fi Speed** with Ethernet also
   connected. The firmware binds the HTTP data socket to the station interface;
   verify actual behavior on your network.
3. Check disconnected Wi-Fi, loss of internet, stop/cancel and scan-in-progress
   behavior. Throughput values must come from your device, not this document.
4. Check LLDP/CDP power details on a known switch that sends the power TLVs.
   Verify that details become unavailable when unplugged or expired.
5. Change text size/contrast, reboot, and confirm the saved setting remains.

Cable length, four-pair wiremap and actual PoE voltage/current/watts cannot be
validated as measurements because this hardware does not measure them.

## Run the host decoder and JSON checks yourself

From the project directory on Ubuntu with GCC installed:

```bash
gcc -std=c11 -Wall -Wextra -Werror -I src src/poe_decode.c tests/test_poe.c -o /tmp/buster-poe-test
/tmp/buster-poe-test
gcc -std=c11 -Wall -Wextra -Werror -Wno-format-truncation -I src src/json.c src/poe_decode.c tests/json_fixture.c -o /tmp/buster-json-test
/tmp/buster-json-test | python3 -m json.tool > /tmp/buster-status-fixture.json
```

## Build environment note

The initial SDK setup in this isolated environment could not use a newer
component-manager process-discovery helper. The build used the SDK-compatible
`idf-component-manager==2.2.0` in the IDF build environment. This did not change
application code or hardware settings. Ordinary Ubuntu process access should
not require that workaround.
