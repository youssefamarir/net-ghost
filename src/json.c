// NetAlly Buster - JSON for the web GUI, Bluetooth and MQTT
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "buster.h"
#include "json.h"

typedef struct { char *b; size_t cap, n; bool first; } jw_t;

static void jw_raw(jw_t *w, const char *fmt, ...)
{
    if (w->n >= w->cap) return;
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(w->b + w->n, w->cap - w->n, fmt, ap);
    va_end(ap);
    if (k > 0) w->n += k;
    if (w->n >= w->cap) w->n = w->cap - 1;
}
static void jw_key(jw_t *w, const char *k) { jw_raw(w, "%s\"%s\":", w->first ? "" : ",", k); w->first = false; }
static void jw_open(jw_t *w, const char *k) { if (k) jw_key(w, k); jw_raw(w, "{"); w->first = true; }
static void jw_close(jw_t *w) { jw_raw(w, "}"); w->first = false; }

void json_esc(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (; src && *src && o + 7 < cap; src++) {
        unsigned char c = *src;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = c; }
        else if (c < 0x20) o += snprintf(dst + o, cap - o, "\\u%04x", c);
        else dst[o++] = c;
    }
    dst[o] = 0;
}
static void jw_s(jw_t *w, const char *k, const char *v)
{
    char e[200]; json_esc(e, sizeof(e), v);
    jw_key(w, k); jw_raw(w, "\"%s\"", e);
}
static void jw_i(jw_t *w, const char *k, long v) { jw_key(w, k); jw_raw(w, "%ld", v); }
static void jw_f(jw_t *w, const char *k, double v, int d) { jw_key(w, k); jw_raw(w, "%.*f", d, v); }
static void jw_b(jw_t *w, const char *k, bool v) { jw_key(w, k); jw_raw(w, v ? "true" : "false"); }

static jw_t jw_new(char *b, size_t cap) { jw_t w = { b, cap, 0, true }; b[0] = 0; return w; }

// ---------------------------------------------------------------------
static void w_link(jw_t *w)
{
    jw_b(w, "link", g.eth_link); jw_b(w, "ip_ok", g.eth_ip);
    jw_i(w, "speed", g.eth_speed); jw_b(w, "full", g.eth_full);
    jw_s(w, "ip", g.ip); jw_s(w, "mask", g.mask); jw_s(w, "gw", g.gw); jw_s(w, "dns", g.dns);
    jw_s(w, "mac", g.mac); jw_s(w, "dhcp", g.dhcp_srv); jw_i(w, "lease", g.lease_s); jw_i(w, "dhcp_ms", g.dhcp_ms);
    jw_i(w, "up_s", g.eth_link && g.link_up_ms ? (long)((now_ms() - g.link_up_ms) / 1000) : 0);
    jw_i(w, "flaps", g.link_flaps);
    jw_i(w, "fps", g.fps); jw_i(w, "bps", g.bps); jw_i(w, "bc", g.bc_pct); jw_i(w, "mc", g.mc_pct);
    jw_i(w, "rx", g.rx_frames); jw_i(w, "rx_bytes", g.rx_bytes);
    jw_i(w, "rx_bc", g.rx_bcast); jw_i(w, "rx_mc", g.rx_mcast); jw_i(w, "rx_arp", g.rx_arp);
    jw_b(w, "blink", g.blinking);
    jw_i(w, "bc_fps", g.bc_fps); jw_i(w, "mc_fps", g.mc_fps); jw_b(w, "hw", g.eth_hw);
    jw_b(w, "static", cfg.static_ip);
}
static void w_quality(jw_t *w)
{
    jw_b(w, "running", g.q_running); jw_i(w, "done", g.q_done); jw_s(w, "target", g.q_target);
    jw_i(w, "sent", g.q_sent); jw_i(w, "recv", g.q_recv);
    jw_i(w, "min", g.q_min); jw_i(w, "avg", g.q_avg); jw_i(w, "max", g.q_max); jw_i(w, "jitter", g.q_jitter);
    jw_f(w, "speed", g.speed_mbps, 2);
}
static void w_batt(jw_t *w)
{
    jw_b(w, "enabled", cfg.battery); jw_b(w, "present", g.batt_present);
    jw_i(w, "pct", g.batt_pct); jw_f(w, "v", g.batt_v, 2); jw_b(w, "charging", g.batt_charging);
}
static void w_alerts(jw_t *w)
{
    bst_lock();
    jw_i(w, "bad", g.alert_bad);
    jw_key(w, "list"); jw_raw(w, "[");
    for (int i = 0; i < g.alert_n; i++) {
        char e[80]; json_esc(e, sizeof(e), g.alerts[i].txt);
        jw_raw(w, "%s{\"l\":%d,\"t\":\"%s\"}", i ? "," : "", g.alerts[i].lvl, e);
    }
    jw_raw(w, "]");
    bst_unlock();
}
static void w_switch(jw_t *w)
{
    bst_lock();
    jw_b(w, "found", g.sw_found); jw_s(w, "proto", g.sw_proto); jw_s(w, "name", g.sw_name);
    jw_s(w, "port", g.sw_port); jw_s(w, "pdesc", g.sw_pdesc); jw_s(w, "vlan", g.sw_vlan);
    jw_s(w, "voice", g.sw_voice); jw_s(w, "mode", g.sw_mode); jw_s(w, "mgmt", g.sw_mgmt);
    jw_s(w, "model", g.sw_model); jw_s(w, "ver", g.sw_ver); jw_s(w, "caps", g.sw_caps);
    jw_s(w, "duplex", g.sw_duplex); jw_s(w, "poe", g.sw_poe); jw_s(w, "chassis", g.sw_chassis);
    jw_s(w, "adv", g.sw_adv); jw_s(w, "vlans_seen", g.vlans_seen);
    jw_i(w, "ttl", g.sw_ttl);
    jw_i(w, "age", g.sw_found ? (long)((now_ms() - g.sw_seen_ms) / 1000) : -1);
    jw_b(w, "stp", g.stp_seen); jw_s(w, "stp_root", g.stp_root); jw_i(w, "stp_cost", g.stp_cost); jw_b(w, "stp_tc", g.stp_tc);
    bst_unlock();
}
static void w_speed(jw_t *w, const speed_result_t *r)
{
    jw_b(w, "running", r->running); jw_b(w, "down_ok", r->down_ok); jw_b(w, "up_ok", r->up_ok);
    jw_i(w, "progress", r->progress); jw_s(w, "stage", r->stage); jw_s(w, "error", r->error);
    jw_f(w, "down", r->down_mbps, 2); jw_f(w, "up", r->up_mbps, 2);
    jw_i(w, "down_bytes", r->down_bytes); jw_i(w, "up_bytes", r->up_bytes);
    jw_i(w, "first_byte", r->first_byte_ms);
    jw_s(w, "network", r->network); jw_s(w, "bssid", r->bssid);
    jw_i(w, "channel", r->channel); jw_i(w, "rssi", r->rssi);
    jw_i(w, "age", r->finished_ms ? (long)((now_ms() - r->finished_ms) / 1000) : -1);
}
static void w_poe(jw_t *w)
{
    const poe_info_t *p = &g.poe;
    jw_b(w, "fresh", poe_is_fresh()); jw_b(w, "advertised", p->valid);
    jw_b(w, "measured", false); jw_s(w, "protocol", p->protocol);
    jw_b(w, "mdi", p->mdi); jw_b(w, "pse", p->pse);
    jw_b(w, "supported", p->supported); jw_b(w, "enabled", p->enabled);
    jw_b(w, "pair_control", p->pair_control); jw_b(w, "extended", p->extended);
    jw_i(w, "pairs", p->pairs); jw_i(w, "class", p->power_class); jw_i(w, "type", p->type);
    jw_i(w, "source", p->source); jw_i(w, "priority", p->priority);
    jw_i(w, "requested_mw", p->requested_mw); jw_i(w, "allocated_mw", p->allocated_mw);
    jw_i(w, "med_mw", p->med_mw); jw_i(w, "available_mw", p->cdp_available_mw); jw_i(w, "used_mw", p->cdp_used_mw);
    jw_i(w, "age", p->valid ? (long)((now_ms() - p->seen_ms) / 1000) : -1);
}
static void w_wifi(jw_t *w, int maxaps)
{
    jw_b(w, "conn", g.wifi_conn); jw_s(w, "cfg", g.w_cfg_ssid);
    jw_s(w, "ssid", g.w_ssid); jw_s(w, "bssid", g.w_bssid); jw_s(w, "ip", g.w_ip); jw_s(w, "gw", g.w_gw);
    jw_i(w, "rssi", g.w_rssi); jw_i(w, "ch", g.w_chan); jw_s(w, "auth", g.w_auth); jw_s(w, "phy", g.w_phy);
    if (maxaps > 0) {
    jw_s(w, "mask", g.w_mask); jw_s(w, "dns", g.w_dns); jw_s(w, "mac", g.w_mac);
    jw_s(w, "country", g.w_country); jw_i(w, "frequency", g.w_freq); jw_i(w, "width", g.w_width);
    jw_i(w, "secondary", g.w_secondary); jw_s(w, "pairwise", g.w_pairwise); jw_s(w, "group", g.w_group);
    jw_b(w, "powersave", g.w_powersave); jw_i(w, "reconnects", g.w_reconnects);
    jw_i(w, "disconnect_reason", g.w_disconnect_reason);
    jw_i(w, "up_s", g.wifi_conn && g.w_connected_ms ? (long)((now_ms() - g.w_connected_ms) / 1000) : 0);
    jw_i(w, "scan_age", g.scan_ms ? (long)((now_ms() - g.scan_ms) / 1000) : -1);
    }
    jw_b(w, "scanning", g.scanning);
    bst_lock();
    jw_i(w, "count", g.ap_count);
    jw_key(w, "aps"); jw_raw(w, "[");
    for (int i = 0; i < g.ap_count && i < maxaps; i++) {
        char e[70]; json_esc(e, sizeof(e), g.aps[i].ssid);
        jw_raw(w, "%s{\"s\":\"%s\",\"b\":\"%s\",\"r\":%d,\"c\":%d,\"a\":\"%s\"}", i ? "," : "",
               e, g.aps[i].bssid, g.aps[i].rssi, g.aps[i].chan, g.aps[i].auth);
    }
    jw_raw(w, "]");
    jw_key(w, "chans"); jw_raw(w, "[");
    for (int c = 1; c <= 13; c++) jw_raw(w, "%s%d", c > 1 ? "," : "", g.chan_cnt[c]);
    jw_raw(w, "]");
    bst_unlock();
}
static void w_test(jw_t *w, bool info)
{
    static const char *nm[TS_COUNT] = {"link", "dhcp", "gateway", "dns", "internet", "switch"};
    jw_b(w, "running", g.test_running); jw_i(w, "runs", g.test_runs); jw_i(w, "result", g.test_result);
    jw_key(w, "steps"); jw_raw(w, "[");
    for (int i = 0; i < TS_COUNT; i++) {
        char e[60]; json_esc(e, sizeof(e), g.ts[i].info);
        if (info) jw_raw(w, "%s{\"n\":\"%s\",\"s\":%d,\"v\":\"%s\",\"i\":\"%s\"}", i ? "," : "", nm[i], g.ts[i].st, g.ts[i].val, e);
        else      jw_raw(w, "%s{\"n\":\"%s\",\"s\":%d,\"v\":\"%s\"}", i ? "," : "", nm[i], g.ts[i].st, g.ts[i].val);
    }
    jw_raw(w, "]");
}
static void w_device(jw_t *w)
{
    jw_s(w, "fw", FW_VERSION); jw_s(w, "node", g.node_id);
    jw_i(w, "up", (long)((now_ms() - g.boot_us / 1000) / 1000));
    jw_f(w, "chip_f", c2f(g.chip_temp), 1);
    jw_i(w, "heap", g.heap_free); jw_i(w, "heap_min", g.heap_min);
    jw_s(w, "reset", g.reset_reason);
    jw_b(w, "ble", g.ble_on); jw_b(w, "ble_conn", g.ble_conn); jw_s(w, "ble_name", g.ble_name);
    jw_b(w, "ble_secure", g.ble_secure); jw_i(w, "ble_bonds", g.ble_bonds); jw_b(w, "ble_pin", cfg.ble_pin);
    jw_i(w, "ap_clients", g.ap_clients); jw_b(w, "mqtt", g.mqtt_conn);
    jw_b(w, "wx", g.wx_valid); jw_i(w, "wx_temp", g.wx_temp); jw_s(w, "wx_cond", g.wx_cond);
    jw_i(w, "epoch", (long)time(NULL));
    jw_b(w, "units_c", cfg.units_c);
    jw_i(w, "text_size", cfg.text_size); jw_b(w, "contrast", cfg.contrast); jw_b(w, "animations", cfg.animations);
}
static void w_gui(jw_t *w)
{
    char u[48];
    jw_s(w, "ssid", g.ap_ssid); jw_s(w, "pass", g.ap_pass);
    snprintf(u, sizeof(u), "http://%s/", g.ap_ip); jw_s(w, "url", u);
    if (g.eth_ip) { snprintf(u, sizeof(u), "http://%s/", g.ip); jw_s(w, "eth_url", u); }
    if (g.wifi_conn) { snprintf(u, sizeof(u), "http://%s/", g.w_ip); jw_s(w, "wifi_url", u); }
    jw_s(w, "ble_url", cfg.ble_url); jw_s(w, "ble_name", g.ble_name);
}

// ---------------------------------------------------------------------
#define SECTION(name, body) int name { jw_t w = jw_new(b, n); jw_open(&w, NULL); body; jw_close(&w); return w.n; }
SECTION(json_link(char *b, size_t n), w_link(&w))
SECTION(json_switch(char *b, size_t n), w_switch(&w))
SECTION(json_wifi(char *b, size_t n, int maxaps), w_wifi(&w, maxaps))
SECTION(json_test(char *b, size_t n, bool info), w_test(&w, info))
SECTION(json_device(char *b, size_t n), w_device(&w))
SECTION(json_gui(char *b, size_t n), w_gui(&w))
SECTION(json_alerts(char *b, size_t n), w_alerts(&w))
SECTION(json_quality(char *b, size_t n), w_quality(&w); jw_open(&w, "batt"); w_batt(&w); jw_close(&w))

int json_summary(char *b, size_t n)
{
    char port[24];
    snprintf(port, sizeof(port), "%s", g.sw_port);
    char e[28]; json_esc(e, sizeof(e), port);
    return snprintf(b, n, "{\"l\":%d,\"ip\":\"%s\",\"sp\":%d,\"sw\":\"%s\",\"v\":\"%s\",\"w\":%d,\"al\":%d,\"at\":%d,\"r\":%d,\"b\":%d}",
                    g.eth_link ? (g.eth_ip ? 2 : 1) : 0, g.ip, g.eth_speed, g.sw_found ? e : "", g.sw_vlan,
                    g.wifi_conn ? g.w_rssi : 0, g.alert_bad, g.test_running ? -1 : g.test_result, g.ap_clients,
                    g.batt_present ? g.batt_pct : -1);
}

int build_status_json(char *b, size_t n)
{
    jw_t w = jw_new(b, n);
    jw_open(&w, NULL);
    jw_s(&w, "name", FW_NAME);
    jw_open(&w, "link"); w_link(&w); jw_close(&w);
    jw_open(&w, "switch"); w_switch(&w); jw_close(&w);
    jw_open(&w, "wifi"); w_wifi(&w, MAX_APS); jw_close(&w);
    jw_open(&w, "test"); w_test(&w, true); jw_close(&w);
    jw_open(&w, "dev"); w_device(&w); jw_close(&w);
    jw_open(&w, "gui"); w_gui(&w); jw_close(&w);
    jw_open(&w, "quality"); w_quality(&w); jw_close(&w);
    jw_open(&w, "wifi_speed"); w_speed(&w, &g.wifi_speed); jw_close(&w);
    jw_open(&w, "eth_speedtest"); w_speed(&w, &g.eth_speedtest); jw_close(&w);
    jw_open(&w, "poe"); w_poe(&w); jw_close(&w);
    jw_open(&w, "cable");
    jw_b(&w, "tdr_supported", false); jw_b(&w, "wiremap_supported", false);
    jw_key(&w, "length_m"); jw_raw(&w, "null");
    jw_s(&w, "data_pairs_reference", "1-2 / 3-6 (10/100BASE-TX); continuity not measured");
    jw_s(&w, "other_pairs_reference", "4-5 / 7-8; continuity not measured");
    jw_close(&w);
    jw_open(&w, "batt"); w_batt(&w); jw_close(&w);
    jw_open(&w, "alerts"); w_alerts(&w); jw_close(&w);
    jw_key(&w, "fps"); jw_raw(&w, "[");
    for (int i = 0; i < 48; i++) jw_raw(&w, "%s%u", i ? "," : "", g.fps_hist[i]);
    jw_raw(&w, "]");
    jw_key(&w, "rssi"); jw_raw(&w, "[");
    for (int i = 0; i < 48; i++) jw_raw(&w, "%s%d", i ? "," : "", g.rssi_hist[i]);
    jw_raw(&w, "]");
    jw_b(&w, "tool", g.tool_running);
    jw_b(&w, "sweep", g.sweep_running); jw_i(&w, "sweep_found", g.sweep_found); jw_i(&w, "sweep_done", g.sweep_done);
    jw_close(&w);
    return w.n;
}

// same keys the SENTRY hub already ingests, plus a few new ones
int json_mqtt(char *b, size_t n)
{
    char sw[48], port[40], ws[33];
    json_esc(sw, sizeof(sw), g.sw_name); json_esc(port, sizeof(port), g.sw_port); json_esc(ws, sizeof(ws), g.w_ssid);
    return snprintf(b, n,
        "{\"node\":\"%s\",\"link\":%s,\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\",\"mac\":\"%s\","
        "\"sw_found\":%s,\"sw_proto\":\"%s\",\"sw_name\":\"%s\",\"sw_port\":\"%s\","
        "\"sw_vlan\":\"%s\",\"sw_mode\":\"%s\",\"sw_mgmt\":\"%s\",\"sw_voice\":\"%s\","
        "\"wifi\":%s,\"wssid\":\"%s\",\"wrssi\":%d,\"wchan\":%d,\"wauth\":\"%s\",\"wip\":\"%s\",\"wbssid\":\"%s\","
        "\"speed\":%d,\"fps\":%u,\"test\":%d,\"alerts\":%d,\"epoch\":%ld,\"uptime\":%ld}",
        g.node_id, g.eth_link ? "true" : "false", g.ip, g.mask, g.gw, g.dns, g.mac,
        g.sw_found ? "true" : "false", g.sw_proto, sw, port, g.sw_vlan, g.sw_mode, g.sw_mgmt, g.sw_voice,
        g.wifi_conn ? "true" : "false", ws, g.w_rssi, g.w_chan, g.w_auth, g.w_ip, g.w_bssid,
        g.eth_speed, (unsigned)g.fps, g.test_result, g.alert_bad, (long)time(NULL), (long)((now_ms() - g.boot_us / 1000) / 1000));
}

int build_scan_json(char *b, size_t n) { return json_wifi(b, n, MAX_APS); }
