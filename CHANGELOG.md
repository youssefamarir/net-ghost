# Net Ghost v2.0

Adds the **switch port VLAN configurator** over SSH.

- In the GUI's new **Config** tab, enter the switch IP, your username/password (never stored),
  and the port. Net Ghost logs in over **SSH** (libssh2 on mbedTLS, on-device), reads the VLAN
  list, and shows them in a dropdown. Pick the target VLAN and it shows the **exact commands**;
  **Apply** asks you to confirm, then pushes them and shows the switch's raw output.
- First vendor: **Ruckus ICX / FastIron** (`conf t / vlan N / untagged ethernet PORT / end / write memory`).
  The template is per-vendor, so others slot in the same way.
- The host and port prefill from CDP/LLDP when the switch advertised them.

**This is the one feature not validated on real hardware here** - SSH prompts and `show vlan`
output vary by firmware, so it captures every raw line from the switch for you to verify, and it
never changes anything without the confirm. Please test it on your Ruckus and tell me what the
switch actually returns; I'll tune the parsing and command set to match.

---

# Net Ghost v1.9

Adds a **device inventory** and smarter security tests on top of v1.8.

- **Find Hosts** is now a real inventory. After the /24 sweep it shows each device's **IP, MAC,
  vendor** (from the MAC) and **name** (reverse DNS when the router provides it) in the phone/
  Bluetooth GUI under Tools - "Devices on the network". Tap Find hosts, then Refresh list.
- Security tests that need Wi-Fi results (**Wi-Fi Audit**, **Rogue AP**) now **run a scan
  themselves** if none has run yet, instead of telling you to do it first.

Next build: the **Ruckus** switch port/VLAN configurator over SSH (its own validated release).

---

# Net Ghost v1.8

Handheld network + security tester for the **Waveshare ESP32-S3-ETH with W5500/PoE module
and 1.28-inch GC9A01/CST816S round touchscreen**. Pins and existing features unchanged.

Start with **[FLASH_UBUNTU.md](FLASH_UBUNTU.md)** for flashing.

## What changed in v1.8

New **Security** column (swipe right past Tools). All of these are read-only audits of your own
network - they look for weak settings, they do not attack, spoof, deauth, or capture anyone's data.

- **Router Scan** - risky open admin services on the gateway (Telnet, FTP, plain-HTTP, ADB, TR-069, RDP).
- **Wi-Fi Audit** - open/WEP/old-WPA networks nearby, WPA3 adoption, your own link's crypto.
- **Rogue AP** - another radio broadcasting your SSID, or an open copy of it (evil twin).
- **DNS Check** - resolver returns the right IP for a known host and rejects fake names (hijack/redirect).
- **SNMP Check** - read-only: does your gear still answer to the default communities public/private.
- **UPnP Scan** - devices that answer UPnP/SSDP and may open ports to the internet by themselves.
- **TLS Cert** - an HTTPS service's cert trust, TLS version and expiry (defaults to the switch/gateway).
- **DHCP Watch** - how many DHCP servers are on the wire (more than one = rogue DHCP), plus the
  standing gateway-MAC-change alert from v1.7.
- **Segment / isolation** - whether something that should be isolated (e.g. switch management) is
  reachable from the port you're on.
- **ARP Test** - the honest ARP-inspection self-test from v1.7 (announces only our own address and
  reports whether the LAN lacks Dynamic ARP Inspection; no spoofing).

Also: **Bufferbloat** added to the Tools column - the extra latency to your gateway while the link
is saturated, graded A-D. This is the one real "speed" number this board measures well.

Every security tool also appears in the phone/Bluetooth GUI under the Tools tab. Tools that take a
target (TLS, Segment, SNMP) default to the switch/gateway on the screen, or take a host in the GUI.

Not built, by design: keyloggers, BadUSB/HID injection, gateway ARP-spoofing, Wi-Fi deauth, or any
tool that captures or redirects someone else's data. Those are attacks, not susceptibility checks.

> **Switch port configuration (SSH) is the next build.** Reconfiguring a switch port's VLAN from the
> device needs an on-board SSH client (libssh2 on mbedTLS) and must be validated against a real
> switch, so it is being done as its own focused release rather than bundled here.

## Columns

DEVICE - SWITCH - WI-FI - TOOLS - SECURITY - SETTINGS - CONNECT. Swipe left/right across the top row
to change column, up/down within one. From a lower page, left/right jumps to the next column's top.

## Hardware limits — what is actually measured

| Item | What this board can report |
|---|---|
| Ethernet link | Negotiated 10/100 Mb/s, duplex, link flaps and frame counters |
| Cable health | Existing packet loss, RTT/jitter and stability test |
| Cable length | **Unavailable**: W5500 has no TDR cable diagnostic engine |
| Four-pair wiremap/continuity | **Unavailable**: no pair-testing hardware |
| Pair information | Pin reference only; not a pass/fail result for individual pairs |
| PoE capability/class/pairs | Switch advertisements, only if an appropriate TLV is heard |
| Requested/allocated/available power | Advertised values, not measured consumption |
| PoE voltage/current/actual watts | **Unavailable**: this PoE module exposes no measurement interface |
| Current input source | **Unknown**: cannot distinguish PoE, USB or battery automatically |
| Wi-Fi | ESP32-S3's 2.4 GHz station connection and nearby 2.4 GHz APs |
| Wi-Fi noise/SNR/current PHY rate | **Not measured**; no inferred values are shown |

The physical PoE module is a power supply, not a programmable power meter.
Neither Ethernet link-up nor an LLDP power advertisement proves the tester is
currently powered by PoE. Missing advertisements do not prove that PoE is absent.
Stale advertisements are hidden when the cable is disconnected or their TTL expires.

## Speed-test behavior

The firmware uses `https://speed.cloudflare.com/__down` and `/__up` for a bounded
**single-connection HTTPS throughput sample on the tester**. It is not an ISP-plan
measurement, a full Wi-Fi benchmark, a PHY link-rate reading or a cable certification.

- `wspeed`: Wi-Fi station only; fails clearly if Wi-Fi is disconnected.
- `espeed`: Ethernet only; fails clearly if Ethernet lacks a usable address.
- `speed`: retains the original auto selection (Ethernet preferred, Wi-Fi otherwise).
- Download limit: 1–20 MB, default 10 MB. Upload: 3 MB.
- Expect roughly 15–30 Mbps on Wi-Fi at a good signal. The ESP32-S3 is a 2.4 GHz, single-antenna
  radio and decrypts HTTPS on its own CPU, so it will not reach a phone or laptop's numbers.
  Ethernet goes through the W5500's SPI bus (now 20 MHz) and tops out around 5–10 Mbps.
- Each payload phase stops after approximately 15 seconds plus a possible current
  socket timeout; connection/header time is additional. A slow download may produce
  a valid time-limited sample. An incomplete upload is not marked successful.
- TLS certificates and HTTP status are checked. Upload requires a full server-
  acknowledged body. Failures do not fabricate zero-speed success results.
- First-byte time includes DNS/connect/TLS/server work; it is not ICMP ping latency.
- Automatic Wi-Fi scans pause during a speed test. Wait for an existing scan to
  finish first. Your phone may stay on the tester's hotspot, but sharing the radio
  adds overhead. BLE, display, TLS, CPU and server/path performance also affect results.
- Stop is available on the screen and GUI. Finished phases may be retained as a
  partial result; errors and result age are visible.

The connected network name and channel are stored with each result. Results are
historical samples; reconnecting does not turn an old sample into a new measurement.

## Web GUI

Tabs: Port, Test, Packets, Wi-Fi, Tools, Device, Cable, PoE. The gear opens Settings
(now with a Bluetooth section). Scroll the bottom tab strip sideways on a narrow phone.

## Existing features retained

LLDP/CDP/FDP discovery; VLANs/voice VLANs; STP; DHCP/static addressing; renew;
AutoTest; 100-ping link quality; frame/protocol counters; packet list; top talkers;
PCAP export; alerts; ping; traceroute; DNS; TCP port test; common-port scan;
subnet sweep; Wake-on-LAN; port blink; Wi-Fi scan/join; hotspot/captive portal;
BLE API and Bluetooth GUI; MQTT; clock/weather; optional battery monitor; settings/factory reset;
ticket report/event log; animated home screen, transitions and touch navigation.

No sensors or new hardware are needed for the software additions. Port blink
temporarily cycles the Ethernet PHY and can interrupt the link. Use USB power
if blinking a PoE-powered port causes a reset, as noted in the original project.

## Original pin assignments — unchanged

| Signal | GPIO |
|---|---:|
| LCD SCLK / MOSI / MISO | 38 / 40 / 39 (MISO unused by panel driver) |
| LCD CS / DC / RST / BL | 42 / 41 / 18 / 15 |
| Touch SDA / SCL / INT / RST | 17 / 16 / 43 / 44 |
| Touch I2C address | `0x15` |
| W5500 SCLK / MISO / MOSI / CS | 13 / 12 / 11 / 14 |
| W5500 INT / RST | 10 / 9 |
| Optional battery ADC / status | 4 / 5; battery monitor remains off until enabled |

These are GPIO numbers, not header positions. Retain your known-working VCC/GND
wiring. The project source does not establish the physical supply connection.

## Build and API

Run `bash build_ubuntu.sh` to build from source with the pinned SDK and regenerate
`src/web_page.h` from `web/index.html`. Do not edit the generated header directly.
Secrets are optional placeholders in `src/secrets.h`; saved NVS values take priority.

`GET /api/status` now also includes `wifi_speed`, `eth_speedtest`, `poe` and `cable`.
Unsupported cable length is JSON `null`. Missing advertised power values are `-1`
and must be gated by `poe.fresh`. No measured-PoE fields contain invented readings.

Commands: `POST /api/tool?type=wspeed`, `type=espeed`, `POST /api/action?do=speedstop`,
`do=bleforget`. Settings keys: `text_size`, `contrast`, `animations`, `speed_mb`, `ble`,
`ble_pin`, `ble_url`. Bluetooth service and characteristics are listed at the top of `src/ble.c`.

See **VALIDATION.md** for the exact checks completed and remaining hardware checks.

## Hardware/API references

- WIZnet W5500 datasheet: https://docs.wiznet.io/img/products/w5500/W5500_ds_v110e.pdf
- Waveshare board schematic: https://files.waveshare.com/wiki/ESP32-S3-ETH/ESP32-S3-ETH-Schematic.pdf
- Espressif HTTP interface binding: https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/protocols/esp_http_client.html
- IEEE 802.3 power TLV reference implementation: https://github.com/lldpd/lldpd/blob/master/src/daemon/protocols/lldp.c
- CDP power TLV reference implementation: https://github.com/wireshark/wireshark/blob/master/epan/dissectors/packet-cdp.c
