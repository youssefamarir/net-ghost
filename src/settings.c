// NetAlly Buster - settings, saved in NVS and editable from the screen, web GUI and Bluetooth
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "nvs.h"
#include "buster.h"
#include "json.h"
#include "secrets.h"

#ifndef AP_PASS
#define AP_PASS "buster1234"
#endif

static const char *TAG = "cfg";
cfg_t cfg;

typedef enum { K_U8, K_U16, K_BOOL, K_STR } ktype_t;
typedef struct { const char *key; ktype_t type; void *ptr; size_t size; int flags; } kdef_t;

static const kdef_t keys[] = {
    { "text_size", K_U8, &cfg.text_size, 0, 0 },
    { "contrast", K_BOOL, &cfg.contrast, 0, 0 },
    { "animations", K_BOOL, &cfg.animations, 0, 0 },
    { "speed_mb", K_U8, &cfg.speed_mb, 0, 0 },
    { "bright",   K_U8,   &cfg.bright,   0, CFG_BRIGHT },
    { "sleep",    K_U16,  &cfg.sleep_s,  0, 0 },
    { "units_c",  K_BOOL, &cfg.units_c,  0, 0 },
    { "autotest", K_BOOL, &cfg.autotest, 0, 0 },
    { "battery",  K_BOOL, &cfg.battery,  0, 0 },
    { "hotspot",  K_BOOL, &cfg.hotspot,  0, CFG_AP },
    { "blink",    K_U8,   &cfg.blink_s,  0, 0 },
    { "ap_pass",  K_STR,  cfg.ap_pass,   sizeof(cfg.ap_pass), CFG_RESTART },
    { "static",   K_BOOL, &cfg.static_ip, 0, CFG_IP },
    { "s_ip",     K_STR,  cfg.s_ip,      sizeof(cfg.s_ip), CFG_IP },
    { "s_mask",   K_STR,  cfg.s_mask,    sizeof(cfg.s_mask), CFG_IP },
    { "s_gw",     K_STR,  cfg.s_gw,      sizeof(cfg.s_gw), CFG_IP },
    { "s_dns",    K_STR,  cfg.s_dns,     sizeof(cfg.s_dns), CFG_IP },
    { "tz",       K_STR,  cfg.tz,        sizeof(cfg.tz), CFG_TZ },
    { "mqtt",     K_STR,  cfg.mqtt,      sizeof(cfg.mqtt), CFG_RESTART },
    { "qhost",    K_STR,  cfg.quality_host, sizeof(cfg.quality_host), 0 },
    { "owm",      K_STR,  cfg.owm,       sizeof(cfg.owm), CFG_RESTART },
    { "ble",      K_BOOL, &cfg.ble,      0, CFG_BLE },
    { "ble_pin",  K_BOOL, &cfg.ble_pin,  0, CFG_BLE },
    { "ble_url",  K_STR,  cfg.ble_url,   sizeof(cfg.ble_url), 0 },
};
#define NKEYS (sizeof(keys) / sizeof(keys[0]))

static bool placeholder(const char *s) { return !s || !s[0] || strstr(s, "your-") == s || strstr(s, "192.168.1.10"); }

static void defaults(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.text_size = 0;
    cfg.contrast = true;
    cfg.animations = true;
    cfg.speed_mb = 10;
    cfg.bright = 80;
    cfg.sleep_s = 0;
    cfg.units_c = false;
    cfg.autotest = true;
    cfg.battery = false;
    cfg.hotspot = true;
    cfg.blink_s = 12;
    cfg.ble = true;
    cfg.ble_pin = true;
    snprintf(cfg.ap_pass, sizeof(cfg.ap_pass), "%s", AP_PASS);
    snprintf(cfg.s_mask, sizeof(cfg.s_mask), "255.255.255.0");
    snprintf(cfg.tz, sizeof(cfg.tz), "EST5EDT,M3.2.0,M11.1.0");
    if (!placeholder(MQTT_BROKER)) snprintf(cfg.mqtt, sizeof(cfg.mqtt), "%s", MQTT_BROKER);
    if (!placeholder(OWM_API_KEY)) snprintf(cfg.owm, sizeof(cfg.owm), "%s", OWM_API_KEY);
}

void cfg_load(void)
{
    defaults();
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return;
    for (size_t i = 0; i < NKEYS; i++) {
        const kdef_t *k = &keys[i];
        switch (k->type) {
        case K_U8:   nvs_get_u8(h, k->key, (uint8_t *)k->ptr); break;
        case K_BOOL: { uint8_t v; if (nvs_get_u8(h, k->key, &v) == ESP_OK) *(bool *)k->ptr = v; } break;
        case K_U16:  nvs_get_u16(h, k->key, (uint16_t *)k->ptr); break;
        case K_STR:  { size_t n = k->size; nvs_get_str(h, k->key, (char *)k->ptr, &n); } break;
        }
    }
    nvs_close(h);
    if (cfg.text_size > 1) cfg.text_size = 1;
    if (cfg.speed_mb < 1) cfg.speed_mb = 1;
    if (cfg.speed_mb > 20) cfg.speed_mb = 20;
    if (cfg.bright < 5) cfg.bright = 5;
    if (cfg.blink_s < 4) cfg.blink_s = 12;
    ESP_LOGI(TAG, "settings loaded");
}

void cfg_save(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return;
    for (size_t i = 0; i < NKEYS; i++) {
        const kdef_t *k = &keys[i];
        switch (k->type) {
        case K_U8:   nvs_set_u8(h, k->key, *(uint8_t *)k->ptr); break;
        case K_BOOL: nvs_set_u8(h, k->key, *(bool *)k->ptr ? 1 : 0); break;
        case K_U16:  nvs_set_u16(h, k->key, *(uint16_t *)k->ptr); break;
        case K_STR:  nvs_set_str(h, k->key, (char *)k->ptr); break;
        }
    }
    nvs_commit(h);
    nvs_close(h);
}

int cfg_set(const char *key, const char *val)
{
    for (size_t i = 0; i < NKEYS; i++) {
        const kdef_t *k = &keys[i];
        if (strcmp(k->key, key)) continue;
        long n = strtol(val, NULL, 10);
        bool b = !strcmp(val, "1") || !strcasecmp(val, "true") || !strcasecmp(val, "on");
        switch (k->type) {
        case K_U8:   if (n < 0) n = 0; if (n > 255) n = 255; *(uint8_t *)k->ptr = n; break;
        case K_U16:  if (n < 0) n = 0; if (n > 65535) n = 65535; *(uint16_t *)k->ptr = n; break;
        case K_BOOL: *(bool *)k->ptr = b; break;
        case K_STR:
            if (!strcmp(key, "ap_pass") && strlen(val) < 8) return 0;   // WPA2 needs 8+
            snprintf((char *)k->ptr, k->size, "%s", val);
            break;
        }
        if (cfg.text_size > 1) cfg.text_size = 1;
        if (cfg.speed_mb < 1) cfg.speed_mb = 1;
        if (cfg.speed_mb > 20) cfg.speed_mb = 20;
        if (cfg.bright < 5) cfg.bright = 5;
        if (cfg.bright > 100) cfg.bright = 100;
        return k->flags | 0x100;   // 0x100 = something changed
    }
    return 0;
}

void cfg_apply(int f)
{
    if (!f) return;
    cfg_save();
    if (f & CFG_BRIGHT) display_set_brightness(cfg.bright);
    if (f & CFG_TZ) { setenv("TZ", cfg.tz, 1); tzset(); }
    if (f & CFG_IP) eth_apply_ip_config();
    if (f & CFG_AP) wifi_hotspot_apply();
    if (f & CFG_BLE) ble_apply();
    ui_post_event(EV_CFG);
    evlog("Settings changed%s", (f & CFG_RESTART) ? " (restart to apply)" : "");
}

int cfg_json(char *b, size_t n)
{
    size_t o = snprintf(b, n, "{");
    for (size_t i = 0; i < NKEYS && o < n; i++) {
        const kdef_t *k = &keys[i];
        const char *sep = i ? "," : "";
        switch (k->type) {
        case K_U8:   o += snprintf(b + o, n - o, "%s\"%s\":%u", sep, k->key, *(uint8_t *)k->ptr); break;
        case K_U16:  o += snprintf(b + o, n - o, "%s\"%s\":%u", sep, k->key, *(uint16_t *)k->ptr); break;
        case K_BOOL: o += snprintf(b + o, n - o, "%s\"%s\":%s", sep, k->key, *(bool *)k->ptr ? "true" : "false"); break;
        case K_STR: {
            char e[200]; json_esc(e, sizeof(e), (char *)k->ptr);
            o += snprintf(b + o, n - o, "%s\"%s\":\"%s\"", sep, k->key, e);
        } break;
        }
    }
    if (o < n) o += snprintf(b + o, n - o, "}");
    return o < n ? (int)o : (int)n - 1;
}

void cfg_factory_reset(void)
{
    nvs_handle_t h;
    const char *ns[] = { "cfg", "wifi" };
    for (int i = 0; i < 2; i++)
        if (nvs_open(ns[i], NVS_READWRITE, &h) == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
    ESP_LOGW(TAG, "factory reset");
    device_reboot();
}
