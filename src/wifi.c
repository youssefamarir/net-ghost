// NetAlly Buster - Wi-Fi: station link, site scanner, and the phone hotspot.
// The hotspot runs a captive portal so a phone that joins it opens the GUI by itself.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "buster.h"
#include "secrets.h"

#ifndef AP_PASS
#define AP_PASS "buster1234"
#endif

static const char *TAG = "wifi";
esp_netif_t *g_sta_netif = NULL, *g_ap_netif = NULL;

static char s_pass[65];
static int64_t s_next_try = 0;
static int s_backoff = 2000;
static bool s_scan_req = false;
static int64_t s_last_scan = -1000000;

static const char *auth_str(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:          return "OPEN";
    case WIFI_AUTH_WEP:           return "WEP";
    case WIFI_AUTH_WPA_PSK:       return "WPA";
    case WIFI_AUTH_WPA2_PSK:      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/2";
    case WIFI_AUTH_WPA3_PSK:      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "ENT";
    default:                      return "?";
    }
}

static const char *cipher_str(wifi_cipher_type_t c)
{
    switch (c) {
    case WIFI_CIPHER_TYPE_NONE: return "None";
    case WIFI_CIPHER_TYPE_WEP40: return "WEP40";
    case WIFI_CIPHER_TYPE_WEP104: return "WEP104";
    case WIFI_CIPHER_TYPE_TKIP: return "TKIP";
    case WIFI_CIPHER_TYPE_CCMP: return "CCMP";
    case WIFI_CIPHER_TYPE_TKIP_CCMP: return "TKIP/CCMP";
    default: return "Other";
    }
}

static bool placeholder(const char *s)
{
    return !s || !s[0] || strstr(s, "your-") == s;
}

// ---------------------------------------------------------------------
// credentials (NVS wins over secrets.h so a tech can change sites from the phone)
// ---------------------------------------------------------------------
static void creds_load(void)
{
    nvs_handle_t h;
    size_t n1 = sizeof(g.w_cfg_ssid), n2 = sizeof(s_pass);
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        bool ok = nvs_get_str(h, "ssid", g.w_cfg_ssid, &n1) == ESP_OK;
        if (ok) nvs_get_str(h, "pass", s_pass, &n2);
        nvs_close(h);
        if (ok) return;
    }
    if (!placeholder(WIFI_SSID)) {
        snprintf(g.w_cfg_ssid, sizeof(g.w_cfg_ssid), "%s", WIFI_SSID);
        snprintf(s_pass, sizeof(s_pass), "%s", WIFI_PASS);
    }
}

static void sta_apply(void)
{
    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", g.w_cfg_ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", s_pass);
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
}

void wifi_set_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass ? pass : "");
        nvs_commit(h);
        nvs_close(h);
    }
    snprintf(g.w_cfg_ssid, sizeof(g.w_cfg_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", pass ? pass : "");
    ESP_LOGI(TAG, "new Wi-Fi network: %s", ssid);
    esp_wifi_disconnect();
    sta_apply();
    s_backoff = 2000;
    s_next_try = now_ms() + 500;
}

// ---------------------------------------------------------------------
// scanning
// ---------------------------------------------------------------------
void wifi_scan_start(void) { s_scan_req = true; }

static void scan_collect(void)
{
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 40) n = 40;
    wifi_ap_record_t *r = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (!r) { esp_wifi_clear_ap_list(); g.scanning = false; return; }
    esp_wifi_scan_get_ap_records(&n, r);   // already sorted strongest first
    bst_lock();
    memset(g.chan_cnt, 0, sizeof(g.chan_cnt));
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (r[i].primary >= 1 && r[i].primary <= 13) g.chan_cnt[r[i].primary]++;
        if (k < MAX_APS) {
            ap_rec_t *a = &g.aps[k++];
            snprintf(a->ssid, sizeof(a->ssid), "%s", (char *)r[i].ssid);
            snprintf(a->bssid, sizeof(a->bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                     r[i].bssid[0], r[i].bssid[1], r[i].bssid[2], r[i].bssid[3], r[i].bssid[4], r[i].bssid[5]);
            a->rssi = r[i].rssi;
            a->chan = r[i].primary;
            snprintf(a->auth, sizeof(a->auth), "%s", auth_str(r[i].authmode));
        }
    }
    g.ap_count = k;
    g.scan_ms = now_ms();
    g.scanning = false;
    bst_unlock();
    free(r);
    ESP_LOGI(TAG, "scan: %d networks", n);
}

static void do_scan(void)
{
    wifi_scan_config_t sc = {0};
    sc.show_hidden = true;
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 60;
    sc.scan_time.active.max = 110;
    if (esp_wifi_scan_start(&sc, false) == ESP_OK) {
        g.scanning = true;
        s_last_scan = now_ms();
    }
}

// ---------------------------------------------------------------------
// events
// ---------------------------------------------------------------------
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (g.w_cfg_ssid[0]) esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        g.wifi_conn = false;
        g.w_connected_ms = 0;
        g.w_ip[0] = g.w_gw[0] = g.w_mask[0] = g.w_dns[0] = 0;
        if (data) g.w_disconnect_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        g.w_reconnects++;
        s_next_try = now_ms() + s_backoff;          // reconnect with backoff
        s_backoff = s_backoff * 2 > 30000 ? 30000 : s_backoff * 2;
        break;
    case WIFI_EVENT_SCAN_DONE:
        scan_collect();
        break;
    case WIFI_EVENT_AP_STACONNECTED:
        g.ap_clients++;
        ui_post_event(EV_PHONE_AP);
        evlog("Phone joined hotspot");
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        if (g.ap_clients > 0) g.ap_clients--;
        break;
    default: break;
    }
    (void)data;
}

static void sta_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    snprintf(g.w_ip, sizeof(g.w_ip), IPSTR, IP2STR(&e->ip_info.ip));
    snprintf(g.w_gw, sizeof(g.w_gw), IPSTR, IP2STR(&e->ip_info.gw));
    snprintf(g.w_mask, sizeof(g.w_mask), IPSTR, IP2STR(&e->ip_info.netmask));
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(g_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        if (dns.ip.type == ESP_IPADDR_TYPE_V4)
            esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, g.w_dns, sizeof(g.w_dns));
        else
            inet_ntop(AF_INET6, dns.ip.u_addr.ip6.addr, g.w_dns, sizeof(g.w_dns));
    }
    g.w_connected_ms = now_ms();
    g.wifi_conn = true;
    s_backoff = 2000;
    ESP_LOGI(TAG, "STA IP %s", g.w_ip);
    evlog("Wi-Fi connected (%s)", g.w_ip);
}

// ---------------------------------------------------------------------
// captive portal DNS: every name resolves to the hotspot address
// ---------------------------------------------------------------------
static void dns_task(void *a)
{
    (void)a;
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(g_ap_netif, &ip);
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = ip.ip.addr };
    if (s < 0 || bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "captive DNS bind failed");
        if (s >= 0) close(s);
        vTaskDelete(NULL);
    }
    uint8_t buf[300];
    while (1) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        // find end of the first question
        int q = 12;
        while (q < n && buf[q]) q += buf[q] + 1;
        q += 5;   // zero byte + qtype + qclass
        if (q > n) continue;
        uint16_t qtype = (buf[q - 4] << 8) | buf[q - 3];
        buf[2] = 0x81; buf[3] = 0x80;              // response, recursion available
        buf[6] = 0; buf[7] = (qtype == 1) ? 1 : 0;  // answers
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = q;
        if (qtype == 1) {
            uint8_t ans[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4};
            memcpy(ans + 12, &ip.ip.addr, 4);
            memcpy(buf + len, ans, 16);
            len += 16;
        }
        sendto(s, buf, len, 0, (struct sockaddr *)&from, fl);
    }
}

// ---------------------------------------------------------------------
// 1 s housekeeping: RSSI, reconnects, periodic scans
// ---------------------------------------------------------------------
static void wifi_task(void *a)
{
    (void)a;
    vTaskDelay(pdMS_TO_TICKS(3000));
    s_scan_req = true;                         // first scan shortly after boot
    while (1) {
        int64_t t = now_ms();
        if (g.wifi_conn) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                g.w_rssi = ap.rssi;
                g.w_chan = ap.primary;
                g.w_freq = ap.primary == 14 ? 2484 : 2407 + 5 * ap.primary;
                g.w_secondary = ap.second == WIFI_SECOND_CHAN_ABOVE ? 1 : ap.second == WIFI_SECOND_CHAN_BELOW ? -1 : 0;
                wifi_bandwidth_t bw = WIFI_BW_HT20;
                esp_wifi_get_bandwidth(WIFI_IF_STA, &bw);
                g.w_width = bw == WIFI_BW_HT40 && g.w_secondary ? 40 : 20;
                snprintf(g.w_pairwise, sizeof(g.w_pairwise), "%s", cipher_str(ap.pairwise_cipher));
                snprintf(g.w_group, sizeof(g.w_group), "%s", cipher_str(ap.group_cipher));
                wifi_ps_type_t ps;
                if (esp_wifi_get_ps(&ps) == ESP_OK) g.w_powersave = ps != WIFI_PS_NONE;
                wifi_country_t country;
                if (esp_wifi_get_country(&country) == ESP_OK)
                    snprintf(g.w_country, sizeof(g.w_country), "%.2s", country.cc);
                snprintf(g.w_ssid, sizeof(g.w_ssid), "%s", (char *)ap.ssid);
                snprintf(g.w_bssid, sizeof(g.w_bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                         ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
                snprintf(g.w_auth, sizeof(g.w_auth), "%s", auth_str(ap.authmode));
                snprintf(g.w_phy, sizeof(g.w_phy), "%s", ap.phy_11ax ? "11ax" : ap.phy_11n ? "11n" : ap.phy_11g ? "11g" : "11b");
            }
        } else if (g.w_cfg_ssid[0] && s_next_try && t >= s_next_try) {
            s_next_try = 0;
            esp_wifi_connect();
        }
        memmove(g.rssi_hist, g.rssi_hist + 1, sizeof(g.rssi_hist) - 1);
        g.rssi_hist[sizeof(g.rssi_hist) - 1] = g.wifi_conn ? (int8_t)g.w_rssi : -100;

        // auto scan every 2 min while no phone is on the hotspot (scans make the AP hop channels)
        if (!g.wifi_speed.running && !g.eth_speedtest.running && !g.scanning && (s_scan_req || (g.ap_clients == 0 && t - s_last_scan > 120000))) {
            s_scan_req = false;
            do_scan();
        }
        if (g.scanning && t - s_last_scan > 8000) g.scanning = false;   // safety
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// hotspot on/off from Settings
void wifi_hotspot_apply(void)
{
    wifi_mode_t m;
    if (esp_wifi_get_mode(&m) != ESP_OK) return;
    wifi_mode_t want = cfg.hotspot ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    if (m != want) {
        esp_wifi_set_mode(want);
        if (!cfg.hotspot) g.ap_clients = 0;
        evlog("Hotspot %s", cfg.hotspot ? "on" : "off");
    }
}

// ---------------------------------------------------------------------
void wifi_start(void)
{
    g_sta_netif = esp_netif_create_default_wifi_sta();
    g_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, sta_got_ip, NULL);

    creds_load();

    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(g.w_mac, sizeof(g.w_mac), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    snprintf(g.ap_ssid, sizeof(g.ap_ssid), "NetGhost-%02X%02X", m[4], m[5]);
    snprintf(g.ap_pass, sizeof(g.ap_pass), "%s", strlen(cfg.ap_pass) >= 8 ? cfg.ap_pass : AP_PASS);
    snprintf(g.node_id, sizeof(g.node_id), "buster-%02x%02x", m[4], m[5]);

    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", g.ap_ssid);
    ap.ap.ssid_len = strlen(g.ap_ssid);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", g.ap_pass);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = strlen(g.ap_pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(cfg.hotspot ? WIFI_MODE_APSTA : WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    sta_apply();

    // hotspot hands out itself as DNS + the captive-portal URL (DHCP option 114)
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(g_ap_netif, &ip);
    snprintf(g.ap_ip, sizeof(g.ap_ip), IPSTR, IP2STR(&ip.ip));
    static char portal_uri[40];
    snprintf(portal_uri, sizeof(portal_uri), "http://%s/", g.ap_ip);
    esp_netif_dns_info_t dns = {0};
    dns.ip.u_addr.ip4.addr = ip.ip.addr;
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    uint8_t offer = 1;   // OFFER_DNS
    esp_netif_dhcps_stop(g_ap_netif);
    esp_netif_dhcps_option(g_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    esp_netif_dhcps_option(g_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, portal_uri, strlen(portal_uri));
    esp_netif_set_dns_info(g_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_start(g_ap_netif);

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "hotspot %s  (pw %s)  station -> %s", g.ap_ssid, g.ap_pass,
             g.w_cfg_ssid[0] ? g.w_cfg_ssid : "(not set)");

    xTaskCreate(dns_task, "cap_dns", 3072, NULL, 4, NULL);
    xTaskCreate(wifi_task, "wifi_mgr", 4096, NULL, 3, NULL);
}
