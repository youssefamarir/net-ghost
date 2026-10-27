// Copy this file to secrets.h and fill in your own values (optional:
// every value here can also be changed later from the phone GUI Settings).
// secrets.h is in .gitignore so it never gets committed.
#pragma once

// Wi-Fi the tester joins for internet/weather (optional - you can also set it from the phone GUI)
#define WIFI_SSID    "your-wifi-name"
#define WIFI_PASS    "your-wifi-password"

// Password for the tester's own hotspot "NetGhost-XXXX" (8+ characters)
#define AP_PASS      "buster1234"

// SENTRY Raspberry Pi hub running Mosquitto (leave as-is to turn MQTT off)
#define MQTT_BROKER  "mqtt://192.168.1.10:1883"

// Free key from https://openweathermap.org/api (leave as-is to turn weather off)
#define OWM_API_KEY  "your-openweathermap-key"
