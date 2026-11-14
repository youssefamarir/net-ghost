

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "buster.h"
#include "lwip/sockets.h"
#include "esp_coexist.h"
#include "esp_wifi.h"

extern esp_netif_t *g_sta_netif, *g_eth_netif;
static bool s_cancel;
void speed_cancel(void) { __atomic_store_n(&s_cancel, true, __ATOMIC_RELAXED); }
static bool cancelled(void) { return __atomic_load_n(&s_cancel, __ATOMIC_RELAXED); }
static bool connected(bool wifi) { return wifi ? g.wifi_conn : g.eth_ip && g.eth_link; }
static void stage(speed_result_t *r, const char *s) { snprintf(r->stage, sizeof(r->stage), "%s", s); }


static int64_t s_breath;
static void breathe(void)
{
    int64_t t = esp_timer_get_time();
    if (t - s_breath > 250000) { s_breath = t; vTaskDelay(1); }
}

void speed_test_run(bool wifi)
{
    speed_result_t *r = wifi ? &g.wifi_speed : &g.eth_speedtest;
    esp_netif_t *netif = wifi ? g_sta_netif : g_eth_netif;
    struct ifreq ifr = {0};
    esp_http_client_handle_t h = NULL;
    char *buf = NULL, url[96];
    memset(r, 0, sizeof(*r));
    r->running = true;
    __atomic_store_n(&s_cancel, false, __ATOMIC_RELAXED);
    stage(r, "Connecting");
    snprintf(r->network, sizeof(r->network), "%s", wifi ? g.w_ssid : g.ip);
    snprintf(r->bssid, sizeof(r->bssid), "%s", wifi ? g.w_bssid : "");
    r->channel = wifi ? g.w_chan : 0;
    r->rssi = wifi ? g.w_rssi : 0;
    if (!connected(wifi) || !netif || esp_netif_get_netif_impl_name(netif, ifr.ifr_name) != ESP_OK) {
        snprintf(r->error, sizeof(r->error), "Connect %s first; no fallback used", wifi ? "Wi-Fi" : "Ethernet");
        goto done;
    }

    if (g.scanning) {
        snprintf(r->error, sizeof(r->error), "Wait for the Wi-Fi scan to finish");
        goto done;
    }


    if (wifi) { esp_coex_preference_set(ESP_COEX_PREFER_WIFI); }
    buf = malloc(16384);
    if (!buf) { snprintf(r->error, sizeof(r->error), "Not enough memory for test buffer"); goto done; }
    unsigned limit = (cfg.speed_mb >= 1 && cfg.speed_mb <= 20 ? cfg.speed_mb : 5) * 1000000U;
    snprintf(url, sizeof(url), "https://speed.cloudflare.com/__down?bytes=%u", limit);
    esp_http_client_config_t c = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 5000, .buffer_size = 8192, .buffer_size_tx = 4096, .if_name = &ifr,
        .disable_auto_redirect = true,
    };
    h = esp_http_client_init(&c);
    if (!h) { snprintf(r->error, sizeof(r->error), "Not enough memory for TLS"); goto done; }
    esp_http_client_set_header(h, "Accept-Encoding", "identity");
    int64_t start = esp_timer_get_time();
    if (esp_http_client_open(h, 0) != ESP_OK) {
        snprintf(r->error, sizeof(r->error), "HTTPS failed; check internet, DNS and device time"); goto done;
    }
    if (esp_http_client_fetch_headers(h) < 0 || esp_http_client_get_status_code(h) != 200) {
        snprintf(r->error, sizeof(r->error), "Download server HTTP %d", esp_http_client_get_status_code(h)); goto done;
    }
    stage(r, "Download");
    int64_t t0 = esp_timer_get_time();
    bool read_error = false;
    while (!cancelled() && connected(wifi) && r->down_bytes < limit && esp_timer_get_time() - t0 < 15000000) {
        unsigned remaining = limit - r->down_bytes;
        int n = esp_http_client_read(h, buf, remaining < 16384 ? remaining : 16384);
        if (n < 0) { read_error = true; break; }
        if (!n) break;
        if (!r->down_bytes) r->first_byte_ms = (int)((esp_timer_get_time() - start) / 1000);
        r->down_bytes += n;
        r->progress = (uint8_t)(r->down_bytes * 65ULL / limit);
        breathe();
    }
    int64_t elapsed = esp_timer_get_time() - t0;
    bool timed = elapsed >= 15000000;
    r->down_ok = !read_error && !cancelled() && connected(wifi) && r->down_bytes > 0 &&
                 (r->down_bytes == limit || timed);
    if (r->down_ok) r->down_mbps = r->down_bytes * 8.0f / elapsed;
    esp_http_client_cleanup(h); h = NULL;
    if (!r->down_ok) {
        snprintf(r->error, sizeof(r->error), "Download interrupted or incomplete"); goto done;
    }
    if (!wifi) g.speed_mbps = r->down_mbps;
    if (cancelled() || !connected(wifi)) goto done;
    stage(r, "Upload");
    r->progress = 65;

    const unsigned upload_limit = 3000000;
    c.url = "https://speed.cloudflare.com/__up";
    c.method = HTTP_METHOD_POST;
    h = esp_http_client_init(&c);
    if (!h) { snprintf(r->error, sizeof(r->error), "Not enough memory for upload TLS"); goto done; }
    esp_http_client_set_header(h, "Content-Type", "application/octet-stream");
    if (esp_http_client_open(h, upload_limit) != ESP_OK) {
        snprintf(r->error, sizeof(r->error), "Upload connection failed; download retained"); goto done;
    }
    esp_fill_random(buf, 16384);
    t0 = esp_timer_get_time();
    while (!cancelled() && connected(wifi) && r->up_bytes < upload_limit && esp_timer_get_time() - t0 < 15000000) {
        unsigned remaining = upload_limit - r->up_bytes;
        int n = esp_http_client_write(h, buf, remaining < 16384 ? remaining : 16384);
        if (n <= 0) break;
        r->up_bytes += n;
        r->progress = 65 + (uint8_t)(r->up_bytes * 34ULL / upload_limit);
        breathe();
    }

    if (!cancelled() && connected(wifi) && r->up_bytes == upload_limit && esp_http_client_fetch_headers(h) >= 0) {
        int status = esp_http_client_get_status_code(h);
        elapsed = esp_timer_get_time() - t0;
        r->up_ok = status >= 200 && status < 300 && elapsed > 0;
        if (r->up_ok) r->up_mbps = r->up_bytes * 8.0f / elapsed;
    }
    if (!r->up_ok) snprintf(r->error, sizeof(r->error), "Upload incomplete/rejected; download retained");
done:
    if (h) esp_http_client_cleanup(h);
    if (wifi) esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    free(buf);
    if (cancelled()) { stage(r, "Cancelled"); snprintf(r->error, sizeof(r->error), "Cancelled; any finished phase is retained"); }
    else if (wifi && strcmp(r->bssid, g.w_bssid)) {
        r->down_ok = r->up_ok = false;
        snprintf(r->error, sizeof(r->error), "Access point changed; run test again"); stage(r, "Failed");
    } else stage(r, r->down_ok && r->up_ok ? "Complete" : r->down_ok ? "Partial" : "Failed");
    r->progress = 100;
    r->finished_ms = now_ms();
    r->running = false;
    evlog("%s speed: %s, down %.2f / up %.2f Mbps", wifi ? "Wi-Fi" : "Ethernet", r->stage, r->down_ok ? r->down_mbps : 0, r->up_ok ? r->up_mbps : 0);
}
