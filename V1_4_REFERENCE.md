# NetAlly Buster v1.4

Handheld network tester firmware for the Waveshare ESP32-S3-ETH (with PoE module) and the 1.28" GC9A01 round touchscreen. Built on the SENTRY prototype firmware, with the temperature and smoke sensors removed. It's a network tester: port, cable, packet, switch and Wi-Fi tools for technicians, plus weather and MQTT to the SENTRY hub.

## Screen pages (swipe left/right)

| # | Page | What it shows |
|---|---|---|
| 1 | Home | Clock, weather, IP and link, switch port/VLAN, Ethernet / Wi-Fi / Bluetooth / battery / alert icons |
| 2 | AutoTest | Link, DHCP, gateway, DNS, internet, switch: pass/warn/fail with timing |
| 3 | Switch | Switch name, port, data + voice VLAN, mode, management IP |
| 4 | Switch info | Model, software, capabilities, switch-side duplex, PoE, port description, STP root |
| 5 | Cable & link | Verdict, both sides' speed/duplex, speeds the switch port supports, drops, loss, latency, jitter, **Test Link** |
| 6 | Port stats | Live frames/s graph, broadcast/multicast %, link uptime, **Blink Port** |
| 7 | Packets | Protocol breakdown (IPv4/IPv6/ARP/L2), DHCP/DNS/mDNS/SSDP/NetBIOS/STP counts, top talker |
| 8 | Alerts | Rogue DHCP, IP conflict, broadcast storm, duplex mismatch, link flapping, STP changes, IPv6 RA, tagged VLANs, IGMP querier, low battery |
| 9 | IP config | Address, mask, gateway, DNS, MAC, DHCP server, lease |
| 10 | Wi-Fi | Signal gauge, SSID, IP, channel, PHY, security |
| 11 | Wi-Fi scan | Strongest networks, open ones flagged, channel load, quietest of 1/6/11 |
| 12 | Device | Uptime, chip temp, memory, battery, reset reason, Bluetooth, hotspot, MQTT |
| 13 | Settings | Brightness, screen sleep, °F/°C, AutoTest on plug, battery fitted, hotspot, restart |
| 14 | Connect | QR code: scan it, the phone joins the hotspot and the GUI opens |

## Phone / laptop GUI

Tabs: **Port** (verdict, AutoTest, cable & link, alerts, switch, IP), **Test**, **Packets** (breakdown, top talkers, live packet list, **.pcap download** for Wireshark), **Wi-Fi** (signal history, scan, join a network), **Tools**, **Device** (status, copy-to-ticket report, event log). The gear opens **Settings**.

Tools: ping, traceroute, DNS lookup, TCP port check, port scan (21 common ports), find hosts on the /24, link quality (100 pings: loss/latency/jitter), internet speed test, Wake-on-LAN, DHCP renew.

Settings (saved on the tester): brightness, screen sleep, units, AutoTest on plug, battery fitted, port blink length, time zone, **static IP**, hotspot on/off and password, SENTRY hub MQTT address, link quality target, weather key, Wi-Fi network, factory reset.

## About the cable tests

The W5500 Ethernet chip can't measure cable length or test individual pairs (that needs a PHY with TDR). Buster checks the signs of a bad cable instead: no link, falling back to 10 Mb or half duplex, a duplex mismatch with the switch (from LLDP/CDP), link drops, and packet loss/jitter under a 100-ping test.

## About the packet view

A switch port only receives broadcasts, multicast, traffic for the tester, and flooded unknown-unicast. To see other devices' traffic, mirror the port (SPAN) on the switch. The .pcap holds the last 64 frames (first 128 bytes each).

## Flash it on Ubuntu

1. Let your user use the USB port, then log out and back in:
   ```
   sudo usermod -aG dialout $USER
   ```
2. Install the flash tool, then close and reopen the terminal:
   ```
   sudo apt install -y pipx && pipx install esptool && pipx ensurepath
   ```
3. Plug in the board, go to the firmware folder, erase and flash:
   ```
   cd netally-buster/firmware
   esptool --chip esp32s3 -p /dev/ttyACM0 erase-flash
   esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 write-flash 0x0 netally-buster-v1.4.0-full.bin
   ```
4. Press RESET on the board.

If it won't connect: hold BOOT, tap RESET, let go of BOOT, run the command again.

To build from source instead: `pipx install platformio`, copy `src/secrets.example.h` to `src/secrets.h`, then `pio run -t erase` and `pio run -t upload` in the project folder.

## Pins (original NetAlly Buster cable)

| Part | Pins |
|---|---|
| GC9A01 display (SPI3) | SCLK 38, MOSI 40, MISO 39 (unused), CS 42, DC 41, RST 18, BL 15 |
| CST816S touch (I2C0, 0x15) | SDA 17, SCL 16, INT 43, RST 44 |
| W5500 Ethernet (SPI2) | SCLK 13, MISO 12, MOSI 11, CS 14, INT 10, RST 9 |
| Battery (optional, later) | GPIO4 divider input, GPIO5 charger status |

## Battery (when you add it)

1-cell Li-ion -> 100k / 100k divider -> **GPIO4**. Charger CHRG/STAT pin (active low, e.g. TP4056) -> **GPIO5**. Then turn on Settings > Battery. Pins and divider ratio are in `src/board.h`.

## Bluetooth LE (for the app)

Advertises as `NetAlly Buster XXXX`, service `c0de0000-4e41-4c4c-5942-555354455221`. Read characteristics return JSON: summary (notify), link, switch, wifi, test, device, alerts, quality+battery, settings, and `0x0020` with the hotspot details and GUI address so the app can join and open the GUI. Write commands to `0x0030` (`autotest`, `quality`, `scan`, `blink`, `renew`, `speed`, `set=<key>=<value>`, `wifi=<ssid>\n<pass>`, ...). Full list at the top of `src/ble.c`.

## Notes

- The board's 8 MB PSRAM is on; GPIO43/44 (UART0) are used by touch, so the console is on USB only.
- Display SPI is 40 MHz like SENTRY; try 80 MHz (`LCD_SPI_HZ`) for smoother animations, back to 40 if it glitches.
- Blink Port powers the Ethernet PHY down and up. If the tester runs on PoE from that same port and resets during a blink, power it from USB-C.
- Edit the web GUI in `web/index.html`, then `python3 tools/make_web.py` to rebuild `src/web_page.h`.

## Project layout

```
src/main.c      start-up, event log, MQTT, weather, device health, screen sleep
src/ui.c        screen GUI (LVGL)            src/display.c   GC9A01 + CST816S + backlight PWM
src/eth.c       W5500, port stats, LLDP/CDP/FDP/STP, static IP, DHCP renew, port blink
src/packets.c   packet analyzer, alerts, live list, .pcap
src/wifi.c      Wi-Fi station, scanner, hotspot + captive portal
src/tools.c     AutoTest, link quality, ping, traceroute, DNS, port check/scan, sweep, speed test, WoL
src/settings.c  saved settings              src/battery.c   battery monitor
src/ble.c       Bluetooth LE service        src/web.c       web server + API + report
src/json.c      JSON for web/BLE/MQTT
web/index.html  phone/laptop GUI source     firmware/       ready-to-flash image
```
