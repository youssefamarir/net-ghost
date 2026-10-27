# Flash Net Ghost v2.0 on Ubuntu

Use the supplied USB-C data port. Leave the original screen wiring as it is.

1. Install Python's virtual-environment support and unzip:

   ```bash
   sudo apt update
   sudo apt install -y python3-venv unzip
   ```

2. Give your user access to serial devices (only required once):

   ```bash
   sudo usermod -aG dialout "$USER"
   ```

   Log out of Ubuntu and log back in after this command. Group membership must
   refresh before your terminal can access the ESP32.

3. Download `net-ghost-v2.0.zip` into Downloads and extract it:

   ```bash
   cd ~/Downloads
   unzip net-ghost-v2.0.zip
   cd net-ghost-v2.0
   ```

4. Plug in the ESP32 using a USB-C **data** cable, close other serial monitors,
   and flash:

   ```bash
   bash flash_ubuntu.sh
   ```

   The script installs esptool inside its own Python environment, verifies the
   supplied binary hashes, identifies an Espressif USB serial port, and writes
   bootloader, partition table and application. It does **not** run erase-flash
   or overwrite the NVS settings partition. Saved Wi-Fi, weather key, hotspot
   password and other saved settings remain in NVS.

   If it asks you to choose a port, list ports and pass the right one:

   ```bash
   ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
   bash flash_ubuntu.sh /dev/ttyACM0
   ```

5. Press RESET if the screen does not restart automatically.

If esptool cannot connect, hold **BOOT**, tap **RESET**, release **BOOT**, and
run the flash command again. Lower `--baud 460800` to `--baud 115200` in
`flash_ubuntu.sh` if your USB connection is unreliable. Do not erase flash to
troubleshoot a connection issue.

## First use

- Swipe left/right for pages; scroll vertically inside the new information pages.
- In **DISPLAY**, tap TEXT for Standard/Large, CONTRAST, ANIMATE or DL LIMIT.
- Open **CONNECT** and scan the hotspot QR code. Join the tester's hotspot.
- Open `http://192.168.4.1` if the captive portal does not open automatically.
- Use **Wi-Fi → Join a network** before running a Wi-Fi speed test. Your phone
  being on the tester's hotspot does not mean the tester has joined upstream Wi-Fi.
- Existing saved settings remain. If a secret was only compiled into the old
  binary and never saved, enter it once in GUI Settings. This source package
  contains placeholder credentials, not extracted private credentials.

## Rebuild after changing source

```bash
bash build_ubuntu.sh
bash flash_ubuntu.sh
```

`build_ubuntu.sh` pins the build to PlatformIO Espressif32 6.12.0 / ESP-IDF 5.5
and LVGL 8.4.0. The first build downloads the compiler and SDK and needs internet.
It regenerates the embedded GUI before compiling and refreshes `firmware/SHA256SUMS`.

For a serial log after flashing:

```bash
.venv-flash/bin/python -m serial.tools.miniterm /dev/ttyACM0 115200
```

Exit the monitor with Ctrl+]. Touch uses GPIO43/44, so debug output stays on
native USB rather than the UART0 pins.
