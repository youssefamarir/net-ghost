// NetAlly Buster - web server: phone/laptop GUI, JSON API, captive-portal redirects
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "buster.h"
#include "web_page.h"

static const char *TAG = "web";
esp_err_t pkt_json_handler(httpd_req_t *req);
esp_err_t pkt_pcap_handler(httpd_req_t *req);

static void nocache(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    nocache(req);
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *req)
{
    char *b = malloc(16384);
    if (!b) return httpd_resp_send_500(req);
    int n = build_status_json(b, 16384);
    httpd_resp_set_type(req, "application/json");
    nocache(req);
    esp_err_t r = httpd_resp_send(req, b, n);
    free(b);
    return r;
}

static esp_err_t h_log(httpd_req_t *req)
{
    char *b = malloc(2100);
    if (!b) return httpd_resp_send_500(req);
    tool_log_copy(b, 2100);
    httpd_resp_set_type(req, "text/plain");
    nocache(req);
    esp_err_t r = httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
    free(b);
    return r;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

static bool qparam(httpd_req_t *req, const char *key, char *out, size_t cap)
{
    char q[200];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, out, cap) != ESP_OK) return false;
    url_decode(out);
    return true;
}

static esp_err_t ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    nocache(req);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// POST /api/action?do=autotest|scan|blink|reboot|sweep
static esp_err_t h_action(httpd_req_t *req)
{
    char a[16] = "";
    qparam(req, "do", a, sizeof(a));
    if (!strcmp(a, "speedstop")) speed_cancel();
    else if (!strcmp(a, "autotest")) autotest_start();
    else if (!strcmp(a, "scan")) wifi_scan_start();
    else if (!strcmp(a, "blink")) eth_blink_start(12);
    else if (!strcmp(a, "sweep")) tool_start("sweep", "");
    else if (!strcmp(a, "quality")) quality_start();
    else if (!strcmp(a, "renew")) eth_dhcp_renew();
    else if (!strcmp(a, "bleforget")) ble_forget_all();
    else if (!strcmp(a, "factory")) { ok(req); cfg_factory_reset(); return ESP_OK; }
    else if (!strcmp(a, "reboot")) { ok(req); device_reboot(); return ESP_OK; }
    else return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown action");
    return ok(req);
}

// POST /api/tool?type=ping|trace|dns|port&arg=host
static esp_err_t h_tool(httpd_req_t *req)
{
    char t[12] = "", arg[64] = "";
    qparam(req, "type", t, sizeof(t));
    qparam(req, "arg", arg, sizeof(arg));
    if (g.tool_running) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "busy");
    tool_start(t, arg);
    return ok(req);
}

// POST /api/wifi  body: ssid=...&pass=...
static esp_err_t h_wifi(httpd_req_t *req)
{
    char body[200] = "", ssid[33] = "", pass[65] = "";
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
    body[n] = 0;
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    url_decode(ssid); url_decode(pass);
    if (!ssid[0]) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
    wifi_set_credentials(ssid, pass);
    return ok(req);
}

// GET /api/settings
static esp_err_t h_settings_get(httpd_req_t *req)
{
    char b[2048];
    int n = cfg_json(b, sizeof(b));
    httpd_resp_set_type(req, "application/json");
    nocache(req);
    return httpd_resp_send(req, b, n);
}

// POST /api/settings  body: key=value&key=value...
static esp_err_t h_settings_post(httpd_req_t *req)
{
    char body[1024];
    int len = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1, got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, body + got, len - got);
        if (n <= 0) break;
        got += n;
    }
    body[got] = 0;
    int flags = 0;
    char *save = NULL;
    for (char *pair = strtok_r(body, "&", &save); pair; pair = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (!eq) continue;
        *eq = 0;
        url_decode(pair); url_decode(eq + 1);
        flags |= cfg_set(pair, eq + 1);
    }
    cfg_apply(flags);
    httpd_resp_set_type(req, "application/json");
    nocache(req);
    return httpd_resp_sendstr(req, (flags & CFG_RESTART) ? "{\"ok\":true,\"restart\":true}" : "{\"ok\":true,\"restart\":false}");
}

// ------- switch configurator -------
static bool form_val(const char *body, const char *key, char *out, size_t cap)
{
    if (httpd_query_key_value(body, key, out, cap) != ESP_OK) { out[0] = 0; return false; }
    url_decode(out);
    return true;
}
static esp_err_t h_switch_get(httpd_req_t *req)
{
    char *b = malloc(2048);
    if (!b) return httpd_resp_send_500(req);
    int n = switch_cfg_json(b, 2048);
    httpd_resp_set_type(req, "application/json");
    nocache(req);
    esp_err_t r = httpd_resp_send(req, b, n);
    free(b);
    return r;
}
static esp_err_t h_switch_connect(httpd_req_t *req)
{
    char body[400]; int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1, got = 0;
    while (got < len) { int k = httpd_req_recv(req, body + got, len - got); if (k <= 0) break; got += k; }
    body[got] = 0;
    char host[40], user[24], pass[64], port[40];
    form_val(body, "host", host, sizeof(host)); form_val(body, "user", user, sizeof(user));
    form_val(body, "pass", pass, sizeof(pass)); form_val(body, "port", port, sizeof(port));
    if (!host[0] || !user[0]) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "host and user required");
    switch_cfg_connect(host, user, pass, port);
    return ok(req);
}
static esp_err_t h_switch_apply(httpd_req_t *req)
{
    char body[400]; int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1, got = 0;
    while (got < len) { int k = httpd_req_recv(req, body + got, len - got); if (k <= 0) break; got += k; }
    body[got] = 0;
    char host[40], user[24], pass[64], port[40], vlan[8];
    form_val(body, "host", host, sizeof(host)); form_val(body, "user", user, sizeof(user));
    form_val(body, "pass", pass, sizeof(pass)); form_val(body, "port", port, sizeof(port));
    form_val(body, "vlan", vlan, sizeof(vlan));
    if (!host[0] || !user[0] || !port[0] || !vlan[0]) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing fields");
    switch_cfg_apply(host, user, pass, port, atoi(vlan));
    return ok(req);
}

// GET /api/hosts - device inventory from the last Find Hosts sweep
static esp_err_t h_hosts(httpd_req_t *req)
{
    char *b = malloc(4096);
    if (!b) return httpd_resp_send_500(req);
    int n = hosts_json(b, 4096);
    httpd_resp_set_type(req, "application/json");
    nocache(req);
    esp_err_t r = httpd_resp_send(req, b, n);
    free(b);
    return r;
}

// GET /api/events
static esp_err_t h_events(httpd_req_t *req)
{
    char *b = malloc(4096);
    if (!b) return httpd_resp_send_500(req);
    int n = evlog_json(b, 4096);
    httpd_resp_set_type(req, "application/json");
    nocache(req);
    esp_err_t r = httpd_resp_send(req, b, n);
    free(b);
    return r;
}

// GET /api/report - plain text summary to paste into a ticket
static esp_err_t h_report(httpd_req_t *req)
{
    static const char *stn[] = { "--", "running", "PASS", "WARN", "FAIL" };
    static const char *tn[] = { "Link", "DHCP", "Gateway", "DNS", "Internet", "Switch" };
    char *b = malloc(8192);
    if (!b) return httpd_resp_send_500(req);
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tm);
    int o = snprintf(b, 8192, "Net Ghost report  %s  (v%s, %s)\n\n", ts, FW_VERSION, g.node_id);
    bst_lock();
    o += snprintf(b + o, 8192 - o, "PORT\n  Link: %s", g.eth_link ? "up" : "DOWN");
    if (g.eth_link) o += snprintf(b + o, 8192 - o, " %d Mb/s %s duplex", g.eth_speed, g.eth_full ? "full" : "half");
    o += snprintf(b + o, 8192 - o, "\n  Switch: %s\n  Port: %s %s\n  VLAN: %s  Voice: %s  Mode: %s\n  Model: %s\n  Mgmt IP: %s  PoE: %s\n",
                  g.sw_found ? g.sw_name : "(not announced)", g.sw_port, g.sw_pdesc[0] ? g.sw_pdesc : "",
                  g.sw_vlan[0] ? g.sw_vlan : "-", g.sw_voice[0] ? g.sw_voice : "-", g.sw_mode, g.sw_model, g.sw_mgmt, g.sw_poe);
    o += snprintf(b + o, 8192 - o, "\nIP\n  Address: %s / %s  Gateway: %s  DNS: %s\n  DHCP server: %s  (%.1f s)\n  Tester MAC: %s\n",
                  g.eth_ip ? g.ip : "-", g.mask, g.gw, g.dns, g.dhcp_srv, g.dhcp_ms / 1000.0f, g.mac);
    o += snprintf(b + o, 8192 - o, "\nAUTOTEST\n");
    for (int i = 0; i < TS_COUNT; i++)
        o += snprintf(b + o, 8192 - o, "  %-9s %-5s %s\n", tn[i], stn[g.ts[i].st], g.ts[i].info);
    if (g.q_sent)
        o += snprintf(b + o, 8192 - o, "\nLINK QUALITY (%s)\n  %d/%d replies, avg %d ms, max %d ms, jitter %d ms\n",
                      g.q_target, g.q_recv, g.q_sent, g.q_avg, g.q_max, g.q_jitter);
    o += snprintf(b + o, 8192 - o, "\nTRAFFIC\n  %u frames/s, broadcast %u/s, multicast %u/s, %u frames seen\n",
                  (unsigned)g.fps, (unsigned)g.bc_fps, (unsigned)g.mc_fps, (unsigned)g.rx_frames);
    o += snprintf(b + o, 8192 - o, "\nALERTS\n");
    if (!g.alert_n) o += snprintf(b + o, 8192 - o, "  none\n");
    for (int i = 0; i < g.alert_n; i++)
        o += snprintf(b + o, 8192 - o, "  [%s] %s\n", g.alerts[i].lvl == 2 ? "!!" : g.alerts[i].lvl == 1 ? "! " : "i ", g.alerts[i].txt);
    o += snprintf(b + o, 8192 - o, "\nWI-FI\n  %s", g.wifi_conn ? g.w_ssid : "not connected");
    if (g.wifi_conn) o += snprintf(b + o, 8192 - o, "  %d dBm  ch %d  %s", g.w_rssi, g.w_chan, g.w_bssid);
    o += snprintf(b + o, 8192 - o, "\n  %d networks in last scan\n", g.ap_count);
    o += snprintf(b + o, 8192 - o, "\nWI-FI DETAILS\n  MAC: %s  Mask: %s  Gateway: %s  DNS: %s\n  Channel: %d  Frequency: %d MHz  Width: %d MHz\n  Security: %s  Cipher: %s / %s\n", g.w_mac, g.wifi_conn ? g.w_mask : "-", g.wifi_conn ? g.w_gw : "-", g.wifi_conn ? g.w_dns : "-", g.wifi_conn ? g.w_chan : 0, g.wifi_conn ? g.w_freq : 0, g.wifi_conn ? g.w_width : 0, g.wifi_conn ? g.w_auth : "-", g.wifi_conn ? g.w_pairwise : "-", g.wifi_conn ? g.w_group : "-");
    speed_result_t *rs[] = { &g.wifi_speed, &g.eth_speedtest };
    for (int i = 0; i < 2; i++) {
        speed_result_t *q = rs[i];
        o += snprintf(b + o, 8192 - o, "\n%s HTTPS SPEED SAMPLE\n  %s: %s\n  Download: %s %.2f Mbps (%u bytes)\n  Upload: %s %.2f Mbps (%u bytes)\n  First byte: %d ms  Network: %s\n  %s\n", i ? "ETHERNET" : "WI-FI", q->running ? "Running" : "Status", q->stage[0] ? q->stage : "Not run", q->down_ok ? "valid" : "unavailable", q->down_ok ? q->down_mbps : 0, (unsigned)q->down_bytes, q->up_ok ? "valid" : "unavailable", q->up_ok ? q->up_mbps : 0, (unsigned)q->up_bytes, q->first_byte_ms, q->network, q->error);
    }
    o += snprintf(b + o, 8192 - o, "\nCABLE\n  Length / pair continuity / wiremap: not measurable on W5500\n  Pair reference only: 1-2 and 3-6 data; 4-5 and 7-8 other pairs.\n\nPOE ADVERTISEMENTS (not measured power)\n  Fresh: %s  Protocol: %s\n", poe_is_fresh() ? "yes" : "no", poe_is_fresh() ? g.poe.protocol : "-");
    if (poe_is_fresh()) {
        const poe_info_t *p = &g.poe;
        o += snprintf(b + o, 8192 - o, "  MDI detail: %s  Role: %s  Enabled: %s  Class: %d  Pair code: %d\n  Advertised mW (-1 = not reported): requested %d, allocated %d, MED %d, CDP available %d, CDP used %d\n", p->mdi ? "yes" : "no", p->mdi ? (p->pse ? "PSE" : "PD") : "unknown", p->mdi ? (p->enabled ? "yes" : "no") : "unknown", p->power_class, p->pairs, p->requested_mw, p->allocated_mw, p->med_mw, p->cdp_available_mw, p->cdp_used_mw);
    }
    o += snprintf(b + o, 8192 - o, "  Voltage / current / consumed watts / input source: not measured.\n\nSpeed results describe this tester's HTTPS throughput, not the ISP plan or Wi-Fi PHY rate.\n");
    bst_unlock();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"net-ghost-report.txt\"");
    nocache(req);
    esp_err_t r = httpd_resp_send(req, b, o < 8192 ? o : 8191);
    free(b);
    return r;
}

// anything unknown -> back to the GUI. This is what makes phones pop the portal open.
static esp_err_t h_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    struct sockaddr_in6 local; socklen_t l = sizeof(local);
    char loc[48] = "/";
    int fd = httpd_req_to_sockfd(req);
    if (getsockname(fd, (struct sockaddr *)&local, &l) == 0) {
        char ip[INET6_ADDRSTRLEN] = "";
        if (local.sin6_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)&local)->sin_addr, ip, sizeof(ip));
        } else {
            // IPv4-mapped IPv6 address: last 4 bytes
            inet_ntop(AF_INET, &local.sin6_addr.un.u32_addr[3], ip, sizeof(ip));
        }
        if (ip[0]) snprintf(loc, sizeof(loc), "http://%s/", ip);
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    nocache(req);
    return httpd_resp_send(req, "Redirect", HTTPD_RESP_USE_STRLEN);
}

void web_start(void)
{
    httpd_handle_t srv = NULL;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 28;
    hc.lru_purge_enable = true;
    hc.stack_size = 6144;
    hc.max_open_sockets = 7;
    if (httpd_start(&srv, &hc) != ESP_OK) { ESP_LOGE(TAG, "httpd failed"); return; }
    const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = h_root },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = h_status },
        { .uri = "/api/log",     .method = HTTP_GET,  .handler = h_log },
        { .uri = "/api/action",  .method = HTTP_POST, .handler = h_action },
        { .uri = "/api/tool",    .method = HTTP_POST, .handler = h_tool },
        { .uri = "/api/wifi",    .method = HTTP_POST, .handler = h_wifi },
        { .uri = "/api/settings", .method = HTTP_GET,  .handler = h_settings_get },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = h_settings_post },
        { .uri = "/api/packets", .method = HTTP_GET,  .handler = pkt_json_handler },
        { .uri = "/api/pcap",    .method = HTTP_GET,  .handler = pkt_pcap_handler },
        { .uri = "/api/events",  .method = HTTP_GET,  .handler = h_events },
        { .uri = "/api/hosts",   .method = HTTP_GET,  .handler = h_hosts },
        { .uri = "/api/switch",          .method = HTTP_GET,  .handler = h_switch_get },
        { .uri = "/api/switch/connect",  .method = HTTP_POST, .handler = h_switch_connect },
        { .uri = "/api/switch/apply",    .method = HTTP_POST, .handler = h_switch_apply },
        { .uri = "/api/report",  .method = HTTP_GET,  .handler = h_report },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(srv, &uris[i]);
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, h_redirect);
    ESP_LOGI(TAG, "web GUI up");
}
