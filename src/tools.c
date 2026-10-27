// NetAlly Buster - AutoTest + technician tools (ping, traceroute, DNS, TCP port, subnet sweep)
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "esp_random.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "esp_netif_net_stack.h"
#include "buster.h"
void json_esc(char *dst, size_t cap, const char *src);

static const char *TAG = "tools";
extern esp_netif_t *g_eth_netif, *g_sta_netif;
static int dns_ptr(uint32_t server, uint32_t ip, bool eth, char *out, size_t cap);

// =====================================================================
// low level helpers
// =====================================================================
static int64_t us(void) { return esp_timer_get_time(); }

static void bind_eth(int s)
{
    if (!g_eth_netif || !g.eth_ip) return;
    struct ifreq ifr = {0};
    if (esp_netif_get_netif_impl_name(g_eth_netif, ifr.ifr_name) == ESP_OK)
        setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr));
}

static bool resolve(const char *host, struct in_addr *out)
{
    if (inet_aton(host, out)) return true;
    struct addrinfo hints = { .ai_family = AF_INET }, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;
    *out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

static uint16_t csum(const void *d, int n)
{
    const uint16_t *p = d; uint32_t s = 0;
    while (n > 1) { s += *p++; n -= 2; }
    if (n) s += *(const uint8_t *)p;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return ~s;
}

typedef struct __attribute__((packed)) { uint8_t type, code; uint16_t sum, id, seq; uint8_t pad[24]; } icmp_t;

static int icmp_open(bool eth, int timeout_ms)
{
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) return -1;
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (eth) bind_eth(s);
    return s;
}

static void icmp_send(int s, uint32_t dst, uint16_t id, uint16_t seq)
{
    icmp_t e = {0};
    e.type = 8; e.id = htons(id); e.seq = htons(seq);
    memcpy(e.pad, "NetGhost-ping-probe-pad01", 24);
    e.sum = csum(&e, sizeof(e));
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_addr.s_addr = dst };
    sendto(s, &e, sizeof(e), 0, (struct sockaddr *)&to, sizeof(to));
}

// wait for a reply belonging to (id, seq). returns ICMP type (0 echo reply, 11 ttl exceeded, 3 unreachable) or -1
static int icmp_wait(int s, uint16_t id, uint16_t seq, int64_t deadline_us, uint32_t *from)
{
    uint8_t buf[128];
    while (us() < deadline_us) {
        struct sockaddr_in fa; socklen_t fl = sizeof(fa);
        int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&fa, &fl);
        if (n <= 0) continue;
        int ihl = (buf[0] & 0x0F) * 4;
        if (n < ihl + 8) continue;
        uint8_t *ic = buf + ihl;
        uint16_t rid, rseq;
        if (ic[0] == 0) { rid = (ic[4] << 8) | ic[5]; rseq = (ic[6] << 8) | ic[7]; }
        else if ((ic[0] == 11 || ic[0] == 3) && n >= ihl + 8 + 20 + 8) {
            uint8_t *inner = ic + 8;
            int ihl2 = (inner[0] & 0x0F) * 4;
            if (n < ihl + 8 + ihl2 + 8) continue;
            uint8_t *ic2 = inner + ihl2;
            rid = (ic2[4] << 8) | ic2[5]; rseq = (ic2[6] << 8) | ic2[7];
        } else continue;
        if (rid != id || rseq != seq) continue;
        if (from) *from = fa.sin_addr.s_addr;
        return ic[0];
    }
    return -1;
}

typedef struct { int sent, recv; int min, max, sum; } pingres_t;

static pingres_t ping_run(uint32_t dst, int count, bool eth, void (*line)(const char *, ...))
{
    pingres_t r = { .min = 99999 };
    int s = icmp_open(eth, 300);
    if (s < 0) return r;
    uint16_t id = (uint16_t)(esp_random() & 0xFFFF);
    for (int i = 0; i < count; i++) {
        int64_t t0 = us();
        icmp_send(s, dst, id, i + 1);
        r.sent++;
        int ty = icmp_wait(s, id, i + 1, t0 + 1000000, NULL);
        int ms = (int)((us() - t0) / 1000);
        if (ty == 0) {
            r.recv++; r.sum += ms;
            if (ms < r.min) r.min = ms;
            if (ms > r.max) r.max = ms;
            if (line) line("reply seq=%d time=%d ms", i + 1, ms);
        } else if (line) line("seq=%d timeout", i + 1);
        int64_t left = 1000000 - (us() - t0);
        if (i + 1 < count && left > 0 && line) vTaskDelay(pdMS_TO_TICKS(left / 1000));
    }
    close(s);
    return r;
}

// raw DNS A query straight to one server (so the test really checks *that* server)
static int dns_query(uint32_t server, const char *name, bool eth, uint32_t *answer, int *rcode)
{
    uint8_t q[300]; int n = 0;
    uint16_t id = esp_random() & 0xFFFF;
    q[n++] = id >> 8; q[n++] = id; q[n++] = 0x01; q[n++] = 0x00;
    q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        int L = dot ? dot - p : (int)strlen(p);
        if (L > 63 || n + L + 6 > (int)sizeof(q)) return -1;
        q[n++] = L; memcpy(q + n, p, L); n += L;
        p += L; if (*p == '.') p++;
    }
    q[n++] = 0; q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1;

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return -1;
    if (eth) bind_eth(s);
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = server };
    int64_t t0 = us();
    sendto(s, q, n, 0, (struct sockaddr *)&to, sizeof(to));
    uint8_t r[512];
    int rl = recv(s, r, sizeof(r), 0);
    int ms = (int)((us() - t0) / 1000);
    close(s);
    if (rl < 12 || r[0] != (id >> 8) || r[1] != (id & 0xFF)) return -1;
    *rcode = r[3] & 0x0F;
    int an = (r[6] << 8) | r[7];
    int i = 12;
    while (i < rl && r[i]) i += r[i] + 1;   // skip question name
    i += 5;
    *answer = 0;
    for (int k = 0; k < an && i + 12 <= rl; k++) {
        if ((r[i] & 0xC0) == 0xC0) i += 2; else { while (i < rl && r[i]) i += r[i] + 1; i++; }
        if (i + 10 > rl) break;
        uint16_t type = (r[i] << 8) | r[i + 1];
        uint16_t dl = (r[i + 8] << 8) | r[i + 9];
        i += 10;
        if (type == 1 && dl == 4 && i + 4 <= rl) { memcpy(answer, r + i, 4); break; }
        i += dl;
    }
    return ms;
}

// TCP connect time (ms) or -1
static int tcp_probe(uint32_t ip, int port, bool eth, int timeout_ms)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return -1;
    if (eth) bind_eth(s);
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip };
    int64_t t0 = us();
    int rc = connect(s, (struct sockaddr *)&to, sizeof(to));
    int ms = -1;
    if (rc == 0) ms = 0;
    else if (errno == EINPROGRESS) {
        fd_set w; FD_ZERO(&w); FD_SET(s, &w);
        struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
        if (select(s + 1, NULL, &w, NULL, &tv) > 0) {
            int err = 0; socklen_t el = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err == 0) ms = (int)((us() - t0) / 1000);
        }
    }
    close(s);
    return ms;
}

// =====================================================================
// AutoTest
// =====================================================================
static void step(int i, tstate_t st, const char *val, const char *info)
{
    g.ts[i].st = st;
    snprintf(g.ts[i].val, sizeof(g.ts[i].val), "%s", val ? val : "");
    snprintf(g.ts[i].info, sizeof(g.ts[i].info), "%s", info ? info : "");
}

static void autotest_task(void *a)
{
    (void)a;
    char b[48], v[24];
    for (int i = 0; i < TS_COUNT; i++) step(i, T_IDLE, "", "");
    g.test_runs++;

    // 1. link
    step(TS_LINK, T_RUN, "", "");
    for (int i = 0; i < 30 && !g.eth_link; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!g.eth_link) {
        step(TS_LINK, T_FAIL, "no link", "No Ethernet link - check cable / port");
        for (int i = TS_DHCP; i < TS_COUNT; i++) step(i, T_FAIL, "--", "skipped (no link)");
        goto done;
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    snprintf(v, sizeof(v), "%dM %s", g.eth_speed, g.eth_full ? "FD" : "HD");
    snprintf(b, sizeof(b), "%d Mb/s %s duplex", g.eth_speed, g.eth_full ? "full" : "half");
    step(TS_LINK, (g.eth_speed >= 100 && g.eth_full) ? T_PASS : T_WARN, v, b);

    // 2. DHCP
    step(TS_DHCP, T_RUN, "", "");
    for (int i = 0; i < 150 && !g.eth_ip && g.eth_link; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!g.eth_ip) {
        step(TS_DHCP, T_FAIL, "no lease", "No DHCP offer in 15 s");
        for (int i = TS_GW; i < TS_NET + 1; i++) step(i, T_FAIL, "--", "skipped (no IP)");
        goto sw;
    }
    if (strncmp(g.ip, "169.254.", 8) == 0) {
        step(TS_DHCP, T_FAIL, "APIPA", "Self-assigned 169.254.x.x - DHCP server not answering");
        goto sw;
    }
    snprintf(v, sizeof(v), "%.1fs", g.dhcp_ms / 1000.0f);
    snprintf(b, sizeof(b), "%s from %s", g.ip, g.dhcp_srv);
    step(TS_DHCP, g.dhcp_ms > 5000 ? T_WARN : T_PASS, v, b);

    // 3. gateway
    step(TS_GW, T_RUN, "", "");
    {
        struct in_addr gw;
        inet_aton(g.gw, &gw);
        pingres_t r = ping_run(gw.s_addr, 4, true, NULL);
        if (r.recv == 0) step(TS_GW, T_FAIL, "no reply", "Gateway did not answer ping");
        else {
            int avg = r.sum / r.recv;
            snprintf(v, sizeof(v), "%d ms", avg);
            snprintf(b, sizeof(b), "%s  %d/%d replies  avg %d ms", g.gw, r.recv, r.sent, avg);
            step(TS_GW, (r.recv < r.sent || avg > 50) ? T_WARN : T_PASS, v, b);
        }
    }

    // 4. DNS
    step(TS_DNS, T_RUN, "", "");
    uint32_t web_ip = 0;
    {
        struct in_addr ds;
        int rc = 0;
        if (!inet_aton(g.dns, &ds) || ds.s_addr == 0) step(TS_DNS, T_FAIL, "no server", "DHCP gave no DNS server");
        else {
            int ms = dns_query(ds.s_addr, "www.google.com", true, &web_ip, &rc);
            if (ms < 0) step(TS_DNS, T_FAIL, "timeout", "DNS server did not answer");
            else if (!web_ip) { snprintf(b, sizeof(b), "rcode %d", rc); step(TS_DNS, T_FAIL, "failed", b); }
            else {
                snprintf(v, sizeof(v), "%d ms", ms);
                snprintf(b, sizeof(b), "%s answered in %d ms", g.dns, ms);
                step(TS_DNS, ms > 200 ? T_WARN : T_PASS, v, b);
            }
        }
    }

    // 5. internet (TCP 443)
    step(TS_NET, T_RUN, "", "");
    {
        uint32_t tgt = web_ip;
        if (!tgt) { struct in_addr x; inet_aton("1.1.1.1", &x); tgt = x.s_addr; }
        int ms = tcp_probe(tgt, 443, true, 3000);
        if (ms < 0) step(TS_NET, T_FAIL, "blocked", "TCP 443 to the internet failed");
        else {
            snprintf(v, sizeof(v), "%d ms", ms);
            snprintf(b, sizeof(b), "HTTPS reachable, connect %d ms", ms);
            step(TS_NET, ms > 150 ? T_WARN : T_PASS, v, b);
        }
    }

sw:
    // 6. switch discovery (give LLDP/CDP up to ~65 s after link up)
    step(TS_SW, T_RUN, "", "");
    while (!g.sw_found && g.eth_link && now_ms() - g.link_up_ms < 65000) vTaskDelay(pdMS_TO_TICKS(250));
    if (g.sw_found) {
        snprintf(v, sizeof(v), "%s v%s", g.sw_proto, g.sw_vlan[0] ? g.sw_vlan : "?");
        snprintf(b, sizeof(b), "%s port %s", g.sw_name, g.sw_port);
        step(TS_SW, T_PASS, v, b);
    } else step(TS_SW, T_WARN, "not seen", "No LLDP/CDP/FDP - unmanaged switch or disabled");

done: {
        int worst = 1;
        for (int i = 0; i < TS_COUNT; i++) {
            if (g.ts[i].st == T_FAIL) worst = 3;
            else if (g.ts[i].st == T_WARN && worst < 2) worst = 2;
        }
        g.test_result = worst;
        g.test_running = false;
        ui_post_event(EV_TEST_DONE);
        ESP_LOGI(TAG, "AutoTest done: %s", worst == 1 ? "PASS" : worst == 2 ? "WARN" : "FAIL");
        evlog("AutoTest %s", worst == 1 ? "passed" : worst == 2 ? "passed with warnings" : "found problems");
    }
    vTaskDelete(NULL);
}

void autotest_start(void)
{
    if (g.test_running) return;
    g.test_running = true;
    if (xTaskCreate(autotest_task, "autotest", 6144, NULL, 4, NULL) != pdPASS) g.test_running = false;
}

// =====================================================================
// tools (results go to a text log the web GUI shows like a terminal)
// =====================================================================
static char s_log[2048];
static int  s_log_len;
static char s_type[12], s_arg[64];

static void tlog(const char *fmt, ...)
{
    char line[128];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    bst_lock();
    int L = strlen(line);
    if (s_log_len + L + 2 >= (int)sizeof(s_log)) {           // drop the oldest half
        int cut = s_log_len / 2;
        while (cut < s_log_len && s_log[cut] != '\n') cut++;
        memmove(s_log, s_log + cut + 1, s_log_len - cut);
        s_log_len -= cut + 1;
    }
    memcpy(s_log + s_log_len, line, L);
    s_log_len += L;
    s_log[s_log_len++] = '\n';
    s_log[s_log_len] = 0;
    bst_unlock();
}

const char *tool_current(void) { return g.tool_running ? s_type : ""; }

void tool_log_copy(char *out, size_t cap)
{
    bst_lock();
    snprintf(out, cap, "%s", s_log);
    bst_unlock();
}

static void tool_ping(const char *host)
{
    struct in_addr a;
    if (!resolve(host, &a)) { tlog("cannot resolve %s", host); return; }
    tlog("PING %s (%s)", host, inet_ntoa(a));
    pingres_t r = ping_run(a.s_addr, 5, false, tlog);
    if (r.recv) tlog("%d/%d received, %d%% loss, min/avg/max %d/%d/%d ms", r.recv, r.sent,
                     (r.sent - r.recv) * 100 / r.sent, r.min, r.sum / r.recv, r.max);
    else tlog("%d sent, 0 received, 100%% loss", r.sent);
}

static void tool_trace(const char *host)
{
    struct in_addr a;
    if (!resolve(host, &a)) { tlog("cannot resolve %s", host); return; }
    tlog("TRACEROUTE %s (%s), 20 hops max", host, inet_ntoa(a));
    int s = icmp_open(false, 300);
    if (s < 0) { tlog("socket error"); return; }
    uint16_t id = esp_random() & 0xFFFF;
    for (int ttl = 1; ttl <= 20; ttl++) {
        setsockopt(s, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
        int64_t t0 = us();
        icmp_send(s, a.s_addr, id, ttl);
        uint32_t from = 0;
        int ty = icmp_wait(s, id, ttl, t0 + 1500000, &from);
        int ms = (int)((us() - t0) / 1000);
        struct in_addr f = { .s_addr = from };
        if (ty < 0) tlog("%2d  *", ttl);
        else tlog("%2d  %-15s %d ms", ttl, inet_ntoa(f), ms);
        if (ty == 0 || ty == 3) break;
    }
    close(s);
    tlog("done");
}

static void tool_dns(const char *host)
{
    uint32_t ans = 0; int rc = 0;
    struct in_addr srv;
    const char *dns = g.eth_ip ? g.dns : "8.8.8.8";
    inet_aton(dns, &srv);
    tlog("DNS %s via %s", host, dns);
    int ms = dns_query(srv.s_addr, host, false, &ans, &rc);
    struct in_addr x = { .s_addr = ans };
    if (ms < 0) tlog("no answer (timeout)");
    else if (!ans) tlog("no A record (rcode %d) in %d ms", rc, ms);
    else tlog("%s -> %s  (%d ms)", host, inet_ntoa(x), ms);
}

static void tool_port(const char *arg)
{
    char host[64]; snprintf(host, sizeof(host), "%s", arg);
    char *c = strrchr(host, ':');
    int port = 80;
    if (c) { *c = 0; port = atoi(c + 1); }
    struct in_addr a;
    if (!resolve(host, &a)) { tlog("cannot resolve %s", host); return; }
    int ms = tcp_probe(a.s_addr, port, false, 3000);
    if (ms >= 0) tlog("TCP %s:%d OPEN (%d ms)", inet_ntoa(a), port, ms);
    else tlog("TCP %s:%d closed / filtered", inet_ntoa(a), port);
}

static void tool_sweep(void)
{
    const char *ip = g.eth_ip ? g.ip : (g.wifi_conn ? g.w_ip : NULL);
    if (!ip) { tlog("no IP address - connect Ethernet or Wi-Fi first"); return; }
    struct in_addr me; inet_aton(ip, &me);
    uint32_t h = ntohl(me.s_addr), base = h & 0xFFFFFF00;
    tlog("SWEEP %u.%u.%u.0/24 (ping)", (unsigned)(base >> 24), (unsigned)((base >> 16) & 255), (unsigned)((base >> 8) & 255));
    g.sweep_running = true; g.sweep_found = 0; g.sweep_done = 0; g.host_n = 0;
    bool eth = g.eth_ip;
    int s = icmp_open(false, 30);
    if (s < 0) { g.sweep_running = false; return; }
    uint16_t id = esp_random() & 0xFFFF;
    static uint8_t seen[32];
    memset(seen, 0, sizeof(seen));
    uint8_t buf[128];
    for (int start = 1; start <= 254 + 8; start += 8) {
        for (int k = start; k < start + 8 && k <= 254; k++) {
            icmp_send(s, htonl(base | k), id, k);
            g.sweep_done = k;
        }
        int64_t until = us() + (start > 254 ? 1500000 : 160000);
        while (us() < until) {
            struct sockaddr_in fa; socklen_t fl = sizeof(fa);
            int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&fa, &fl);
            if (n <= 0) continue;
            int ihl = (buf[0] & 0x0F) * 4;
            if (n < ihl + 8 || buf[ihl] != 0) continue;
            uint16_t rid = (buf[ihl + 4] << 8) | buf[ihl + 5];
            if (rid != id) continue;
            uint32_t from = ntohl(fa.sin_addr.s_addr);
            if ((from & 0xFFFFFF00) != base) continue;
            int last = from & 255;
            if (seen[last >> 3] & (1 << (last & 7))) continue;
            seen[last >> 3] |= 1 << (last & 7);
            g.sweep_found++;
            if (g.host_n < MAX_HOSTS) { memset(&g.hosts[g.host_n], 0, sizeof(host_t)); g.hosts[g.host_n].ip = fa.sin_addr.s_addr; g.host_n++; }
            tlog("  %s%s", inet_ntoa(fa.sin_addr), from == h ? "  (this tester)" : (strcmp(inet_ntoa(fa.sin_addr), g.gw) == 0 ? "  (gateway)" : ""));
        }
    }
    close(s);
    // enrich each host with its MAC (from the ARP cache the pings filled) and a reverse-DNS name
    struct netif *lw = esp_netif_get_netif_impl(eth ? g_eth_netif : g_sta_netif);
    struct in_addr dsrv; const char *dns = eth ? g.dns : "8.8.8.8";
    bool have_dns = inet_aton(dns, &dsrv) && dsrv.s_addr;
    for (int i = 0; i < g.host_n; i++) {
        host_t *hh = &g.hosts[i];
        if (lw) {
            ip4_addr_t a4; a4.addr = hh->ip;
            struct eth_addr *ea = NULL; const ip4_addr_t *ipr = NULL;
            if (etharp_find_addr(lw, &a4, &ea, &ipr) >= 0 && ea) memcpy(hh->mac, ea->addr, 6);
        }
        if (have_dns && i < 24) dns_ptr(dsrv.s_addr, hh->ip, eth, hh->name, sizeof(hh->name));
    }
    tlog("%d hosts answered ping (see Devices in the GUI)", g.sweep_found);
    g.sweep_running = false;
}

// ---------------------------------------------------------------------
// link quality: 100 pings at 10/s -> loss, latency, jitter
// ---------------------------------------------------------------------
static void quality_task(void *a)
{
    (void)a;
    const char *target = cfg.quality_host[0] ? cfg.quality_host : g.gw;
    struct in_addr dst;
    g.q_sent = g.q_recv = g.q_min = g.q_avg = g.q_max = g.q_jitter = 0;
    g.q_done = 0;
    snprintf(g.q_target, sizeof(g.q_target), "%s", target);
    if (!g.eth_ip || !resolve(target, &dst)) {
        tlog("link quality: no IP / target");
        g.q_running = false;
        vTaskDelete(NULL);
    }
    tlog("LINK QUALITY to %s (100 pings, 10/s)", target);
    int s = icmp_open(true, 50);
    if (s < 0) { g.q_running = false; vTaskDelete(NULL); }
    uint16_t id = esp_random() & 0xFFFF;
    int mn = 99999, mx = 0, sum = 0, prev = -1;
    long jsum = 0; int jn = 0;
    for (int i = 0; i < 100; i++) {
        int64_t t0 = us();
        icmp_send(s, dst.s_addr, id, i + 1);
        g.q_sent++;
        if (icmp_wait(s, id, i + 1, t0 + 500000, NULL) == 0) {
            int rtt = (int)((us() - t0) / 1000);
            g.q_recv++; sum += rtt;
            if (rtt < mn) mn = rtt;
            if (rtt > mx) mx = rtt;
            if (prev >= 0) { jsum += abs(rtt - prev); jn++; }
            prev = rtt;
            g.q_min = mn; g.q_max = mx; g.q_avg = sum / g.q_recv;
            g.q_jitter = jn ? (int)(jsum / jn) : 0;
        }
        g.q_done = i + 1;
        int64_t left = 100000 - (us() - t0);
        if (left > 1000) vTaskDelay(pdMS_TO_TICKS(left / 1000));
    }
    close(s);
    int loss = (g.q_sent - g.q_recv) * 100 / g.q_sent;
    tlog("%d/%d replies, %d%% loss, min/avg/max %d/%d/%d ms, jitter %d ms", g.q_recv, g.q_sent, loss,
         g.q_recv ? g.q_min : 0, g.q_avg, g.q_max, g.q_jitter);
    tlog("");
    evlog("Link quality: %d%% loss, avg %d ms, jitter %d ms", loss, g.q_avg, g.q_jitter);
    g.q_running = false;
    ui_post_event(EV_QUALITY_DONE);
    vTaskDelete(NULL);
}

void quality_start(void)
{
    if (g.q_running) return;
    g.q_running = true;
    if (xTaskCreate(quality_task, "quality", 4096, NULL, 4, NULL) != pdPASS) g.q_running = false;
}

// ---------------------------------------------------------------------
// internet download speed (HTTPS, through the Ethernet port when it has an IP)
// ---------------------------------------------------------------------
static void tool_speed(bool wifi)
{
    speed_test_run(wifi);
    speed_result_t *r = wifi ? &g.wifi_speed : &g.eth_speedtest;
    tlog("%s SPEED SAMPLE: %s", wifi ? "WI-FI" : "ETHERNET", r->stage);
    if (r->down_ok) tlog("Download %.2f Mbps (%u bytes), first byte %d ms", r->down_mbps, (unsigned)r->down_bytes, r->first_byte_ms);
    if (r->up_ok) tlog("Upload %.2f Mbps (%u bytes, server acknowledged)", r->up_mbps, (unsigned)r->up_bytes);
    if (r->error[0]) tlog("%s", r->error);
    tlog("HTTPS sample of THIS tester, not a measurement of your ISP plan or Wi-Fi PHY rate.");
}

// ---------------------------------------------------------------------
// common TCP ports on one host
// ---------------------------------------------------------------------
// =====================================================================
// network device inventory (for Find Hosts): IP, MAC, vendor, name
// =====================================================================

// A tiny OUI table for the devices people most often have at home.
const char *mac_vendor(const uint8_t *m)
{
    struct { uint8_t p[3]; const char *n; } t[] = {
        {{0x3C,0x22,0xFB},"Apple"}, {{0xF0,0x18,0x98},"Apple"}, {{0xA4,0x83,0xE7},"Apple"}, {{0xDC,0xA6,0x32},"Raspberry Pi"},
        {{0xB8,0x27,0xEB},"Raspberry Pi"}, {{0xE4,0x5F,0x01},"Raspberry Pi"}, {{0x44,0x07,0x0B},"Google"}, {{0x1C,0x53,0xF9},"Amazon"},
        {{0xFC,0xA1,0x83},"Amazon"}, {{0x68,0x54,0xFD},"Amazon"}, {{0x50,0xDC,0xE7},"Amazon"}, {{0x8C,0x85,0x90},"Apple"},
        {{0x00,0x1A,0x11},"Google"}, {{0xDA,0xA1,0x19},"Google"}, {{0x54,0x60,0x09},"Google"}, {{0xAC,0x63,0xBE},"Amazon"},
        {{0x00,0x17,0x88},"Philips Hue"}, {{0xEC,0xFA,0xBC},"Espressif"}, {{0x24,0x0A,0xC4},"Espressif"}, {{0x7C,0xDF,0xA1},"Espressif"},
        {{0x34,0xAB,0x95},"Espressif"}, {{0x48,0x27,0xE2},"Espressif"}, {{0x00,0x50,0x56},"VMware"}, {{0x00,0x0C,0x29},"VMware"},
        {{0x00,0x1C,0x14},"Cisco"}, {{0x00,0x1B,0x0D},"Cisco"}, {{0xC0,0xC5,0x20},"Ruckus"}, {{0x24,0xC9,0xA1},"Ruckus"},
        {{0x58,0x93,0x96},"Ruckus"}, {{0x2C,0xC5,0xD3},"Ruckus"}, {{0x00,0x15,0x5D},"Microsoft"}, {{0xB4,0x2E,0x99},"Intel"},
        {{0x50,0xEB,0x71},"Intel"}, {{0x3C,0x7C,0x3F},"ASUSTek"}, {{0xD8,0x0D,0x17},"TP-Link"}, {{0x50,0xC7,0xBF},"TP-Link"},
        {{0xFC,0xEC,0xDA},"Ubiquiti"}, {{0x74,0xAC,0xB9},"Ubiquiti"}, {{0x78,0x8A,0x20},"Ubiquiti"}, {{0x18,0xE8,0x29},"Ubiquiti"},
        {{0x00,0x1D,0xD8},"Microsoft"}, {{0xB8,0x27,0xEB},"Raspberry Pi"}, {{0xD4,0x6E,0x0E},"TP-Link"}, {{0x54,0xAF,0x97},"Samsung"},
        {{0x78,0xBD,0xBC},"Samsung"}, {{0x5C,0x49,0x7D},"Samsung"}, {{0x08,0x00,0x27},"VirtualBox"},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!memcmp(m, t[i].p, 3)) return t[i].n;
    return "";
}

// Reverse-DNS (PTR) name for an IP, best effort. Returns name length or 0.
static int dns_ptr(uint32_t server, uint32_t ip, bool eth, char *out, size_t cap)
{
    out[0] = 0;
    uint8_t q[64]; int n = 0;
    uint16_t id = esp_random() & 0xFFFF;
    q[n++] = id >> 8; q[n++] = id & 0xFF; q[n++] = 0x01; q[n++] = 0x00;
    q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0;
    uint8_t *b = (uint8_t *)&ip;                 // build d.c.b.a.in-addr.arpa
    for (int k = 3; k >= 0; k--) { char lbl[4]; int L = snprintf(lbl, sizeof(lbl), "%d", b[k]); q[n++] = L; memcpy(q + n, lbl, L); n += L; }
    const char *suf[] = { "in-addr", "arpa" };
    for (int k = 0; k < 2; k++) { int L = strlen(suf[k]); q[n++] = L; memcpy(q + n, suf[k], L); n += L; }
    q[n++] = 0; q[n++] = 0; q[n++] = 12; q[n++] = 0; q[n++] = 1;   // QTYPE PTR, QCLASS IN
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return 0;
    if (eth) bind_eth(s);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 600000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = server };
    sendto(s, q, n, 0, (struct sockaddr *)&to, sizeof(to));
    uint8_t r[256];
    int rl = recv(s, r, sizeof(r), 0);
    close(s);
    if (rl < 12 || (r[3] & 0x0F)) return 0;
    int an = (r[6] << 8) | r[7];
    int i = 12;
    while (i < rl && r[i]) i += r[i] + 1;
    i += 5;                                                  // skip question
    for (int k = 0; k < an && i + 12 <= rl; k++) {
        if ((r[i] & 0xC0) == 0xC0) i += 2; else { while (i < rl && r[i]) i += r[i] + 1; i++; }
        uint16_t type = (r[i] << 8) | r[i + 1];
        uint16_t dl = (r[i + 8] << 8) | r[i + 9];
        i += 10;
        if (type == 12) {                                     // PTR: decode the name
            int j = i, o = 0, guard = 0;
            while (j < rl && r[j] && guard++ < 20) {
                if ((r[j] & 0xC0) == 0xC0) { j = ((r[j] & 0x3F) << 8) | r[j + 1]; continue; }
                int L = r[j++];
                for (int x = 0; x < L && j < rl && o < (int)cap - 2; x++) out[o++] = r[j++];
                out[o++] = '.';
            }
            if (o) o--;
            out[o] = 0;
            return o;
        }
        i += dl;
    }
    return 0;
}

int hosts_json(char *b, size_t cap)
{
    int o = snprintf(b, cap, "{\"n\":%d,\"list\":[", g.host_n);
    for (int i = 0; i < g.host_n && o < (int)cap - 120; i++) {
        host_t *h = &g.hosts[i];
        struct in_addr a = { .s_addr = h->ip };
        char mac[18]; snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                               h->mac[0], h->mac[1], h->mac[2], h->mac[3], h->mac[4], h->mac[5]);
        bool nomac = !(h->mac[0] | h->mac[1] | h->mac[2] | h->mac[3] | h->mac[4] | h->mac[5]);
        char nm[56]; json_esc(nm, sizeof(nm), h->name);
        o += snprintf(b + o, cap - o, "%s{\"ip\":\"%s\",\"mac\":\"%s\",\"vendor\":\"%s\",\"name\":\"%s\"}",
                      i ? "," : "", inet_ntoa(a), nomac ? "" : mac, nomac ? "" : mac_vendor(h->mac), nm);
    }
    o += snprintf(b + o, cap - o, "]}");
    return o;
}

// =====================================================================
// SECURITY CHECKS (home-network hygiene, read-only).
// These look for weak settings and report them. They do not attack anything.
// One runs at a time (serialized by tool_running). Each fills its own result
// so its screen page and the web GUI can show it live.
// =====================================================================
static void sec_line(struct secres *r, const char *fmt, ...);
// If the Wi-Fi tools have no scan results yet, run a scan and wait for it.
static void ensure_wifi_scan(struct secres *r)
{
    if (g.ap_count > 0) return;
    if (r) { snprintf(r->verdict, sizeof(r->verdict), "SCANNING..."); sec_line(r, "Running a Wi-Fi scan..."); r->done = 10; }
    wifi_scan_start();
    for (int i = 0; i < 15 && !g.scanning; i++) vTaskDelay(pdMS_TO_TICKS(100));   // wait for it to begin
    for (int i = 0; i < 90 && g.scanning; i++) vTaskDelay(pdMS_TO_TICKS(100));    // up to ~9 s to finish
    vTaskDelay(pdMS_TO_TICKS(300));
}

static void sec_line(struct secres *r, const char *fmt, ...)
{
    if (r->n >= 6) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(r->line[r->n], sizeof(r->line[0]), fmt, ap);
    va_end(ap);
    r->n++;
}
static void sec_begin(struct secres *r) { memset(r, 0, sizeof(*r)); r->running = true; }
static void sec_end(struct secres *r, int level, const char *verdict)
{
    r->level = level;
    snprintf(r->verdict, sizeof(r->verdict), "%s", verdict);
    r->done = 100;
    r->running = false;
}
static void sec_report(struct secres *r, const char *title)
{
    tlog("%s: %s", title, r->verdict);
    for (int i = 0; i < r->n; i++) tlog("  %s", r->line[i]);
    tlog("");
}

// --- Router / switch exposure: which risky admin services answer on the gateway
static void sec_router_scan(void)
{
    struct secres *r = &g.sec_router;
    sec_begin(r);
    const char *ip = g.eth_ip ? g.gw : (g.wifi_conn ? g.w_gw : NULL);
    struct in_addr a;
    if (!ip || !ip[0] || !resolve(ip, &a)) { sec_end(r, 1, "NO GATEWAY"); return; }
    bool eth = g.eth_ip;
    static const struct { int p; const char *n; int sev; } svc[] = {
        {23,"Telnet",2}, {2323,"Telnet",2}, {5555,"ADB",2}, {9527,"debug",2},
        {21,"FTP",1}, {80,"HTTP admin",1}, {8080,"HTTP",1}, {7547,"TR-069 WAN",1}, {37777,"DVR",1}, {3389,"RDP",1},
        {22,"SSH",0}, {443,"HTTPS",0},
    };
    int N = sizeof(svc) / sizeof(svc[0]), worst = 0, risky = 0;
    for (int i = 0; i < N; i++) {
        int ms = tcp_probe(a.s_addr, svc[i].p, eth, 500);
        r->done = (i + 1) * 90 / N;
        if (ms < 0) continue;
        if (svc[i].sev == 0) { sec_line(r, "%d %s open (ok)", svc[i].p, svc[i].n); continue; }
        sec_line(r, "%d %s OPEN - risky", svc[i].p, svc[i].n);
        risky++;
        if (svc[i].sev > worst) worst = svc[i].sev;
    }
    if (!risky) sec_line(r, "No risky admin ports open");
    sec_line(r, "Gateway %s", ip);
    if (worst == 2) sec_end(r, 2, "EXPOSED");
    else if (worst == 1) sec_end(r, 1, "REVIEW THESE");
    else sec_end(r, 0, "LOOKS OK");
    evlog("Router check: %s (%d risky ports)", r->verdict, risky);
}

// --- Wi-Fi hygiene from the latest scan results (encryption, open/WEP, our network)
static void sec_wifi_scan(void)
{
    struct secres *r = &g.sec_wifi;
    sec_begin(r);
    ensure_wifi_scan(r);
    if (g.ap_count == 0) { sec_line(r, "No networks found"); sec_end(r, 1, "NO WI-FI"); return; }
    int open = 0, wep = 0, wpa3 = 0, worst = 0;
    for (int i = 0; i < g.ap_count; i++) {
        const char *au = g.aps[i].auth;
        if (!strcmp(au, "OPEN")) open++;
        else if (!strcmp(au, "WEP") || !strcmp(au, "WPA")) wep++;
        else if (strstr(au, "WPA3")) wpa3++;
    }
    r->done = 60;
    if (open) { sec_line(r, "%d open network%s nearby", open, open == 1 ? "" : "s"); if (worst < 1) worst = 1; }
    if (wep)  { sec_line(r, "%d on WEP/old WPA (weak)", wep); worst = 2; }
    sec_line(r, "%d of %d use WPA3", wpa3, g.ap_count);
    if (g.wifi_conn) {
        if (!strcmp(g.w_auth, "OPEN")) { sec_line(r, "THIS link is OPEN (no crypto)"); worst = 2; }
        else if (!strcmp(g.w_auth, "WEP") || !strcmp(g.w_auth, "WPA")) { sec_line(r, "THIS link is weak: %s", g.w_auth); worst = 2; }
        else sec_line(r, "This link: %s %s", g.w_auth, g.w_pairwise);
    }
    r->done = 90;
    if (worst == 2) sec_end(r, 2, "WEAK CRYPTO");
    else if (worst == 1) sec_end(r, 1, "OPEN APS NEAR");
    else sec_end(r, 0, "ENCRYPTED");
    evlog("Wi-Fi check: %s (%d open, %d weak)", r->verdict, open, wep);
}

// --- DNS integrity: does the DHCP-provided resolver answer honestly?
static void sec_dns_scan(void)
{
    struct secres *r = &g.sec_dns;
    sec_begin(r);
    bool eth = g.eth_ip;
    const char *dnss = eth ? g.dns : "8.8.8.8";
    struct in_addr ds;
    if (!dnss[0] || !inet_aton(dnss, &ds) || !ds.s_addr) { sec_line(r, "No DNS server"); sec_end(r, 1, "NO DNS"); return; }
    sec_line(r, "Resolver %s", dnss);
    int worst = 0, rc = 0; uint32_t ip = 0;
    r->done = 30;
    int ms = dns_query(ds.s_addr, "one.one.one.one", eth, &ip, &rc);
    if (ms < 0) { sec_line(r, "No answer (timeout)"); worst = 1; }
    else {
        struct in_addr x = { .s_addr = ip };
        char *d = inet_ntoa(x);
        if (ip && (!strcmp(d, "1.1.1.1") || !strcmp(d, "1.0.0.1"))) sec_line(r, "Known host OK (%d ms)", ms);
        else { sec_line(r, "Wrong IP for known host: %s", ip ? d : "none"); worst = 2; }
    }
    r->done = 70;
    char junk[40]; snprintf(junk, sizeof(junk), "nx%08lx.example.invalid", (unsigned long)(esp_random()));
    ip = 0; rc = 0;
    ms = dns_query(ds.s_addr, junk, eth, &ip, &rc);
    if (ms >= 0 && ip) { struct in_addr x = { .s_addr = ip }; sec_line(r, "Fake name resolved to %s", inet_ntoa(x)); worst = 2; }
    else sec_line(r, "Bad names correctly rejected");
    if (worst == 2) sec_end(r, 2, "DNS SUSPECT");
    else if (worst == 1) sec_end(r, 1, "NO ANSWER");
    else sec_end(r, 0, "DNS HONEST");
    evlog("DNS check: %s", r->verdict);
}

// --- ARP inspection self-test.
// We announce OUR OWN address with a gratuitous ARP and watch what happens. If the
// network accepts an unsolicited ARP without challenge and shows no sign of Dynamic
// ARP Inspection, it is susceptible to ARP spoofing. We never claim another device's
// address and never redirect anyone's traffic, so this reports susceptibility without
// performing an attack.
void sec_arp_selftest(void)
{
    struct secres *r = &g.sec_arp;
    sec_begin(r);
    esp_netif_t *netif = g.eth_ip ? g_eth_netif : (g.wifi_conn ? g_sta_netif : NULL);
    struct netif *lw = netif ? esp_netif_get_netif_impl(netif) : NULL;
    if (!lw) { sec_line(r, "Connect Ethernet or Wi-Fi first"); sec_end(r, 1, "NO LINK"); return; }
    sec_line(r, "Announcing our own address...");
    r->done = 20;
    bool conflict_before = g.ip_conflict[0] != 0;
    bool sent = etharp_gratuitous(lw) == ERR_OK;
    r->done = 40;
    bool defended = false, linkdrop = false;
    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        r->done = 40 + i * 2;
        if (!conflict_before && g.ip_conflict[0]) defended = true;
        if (g.eth_ip && !g.eth_link) linkdrop = true;
    }
    sec_line(r, sent ? "Gratuitous ARP accepted" : "Could not send ARP");
    sec_line(r, g.gw_mac[0] ? "Gateway %s" : "Gateway MAC unknown", g.gw_mac);
    if (linkdrop) { sec_line(r, "Port shut off - DAI present (good)"); sec_end(r, 0, "PROTECTED"); }
    else if (defended) { sec_line(r, "Address is defended on this LAN"); sec_end(r, 1, "DEFENDED"); }
    else {
        sec_line(r, "No Dynamic ARP Inspection seen");
        sec_line(r, "Fix: DAI + DHCP snooping on switch");
        sec_end(r, 2, "SUSCEPTIBLE");
    }
    evlog("ARP inspection test: %s", r->verdict);
}

// --- Rogue AP / evil-twin: another radio broadcasting our SSID, or an open copy
static void sec_rogue_scan(void)
{
    struct secres *r = &g.sec_rogue;
    sec_begin(r);
    if (!g.wifi_conn) { sec_line(r, "Join your Wi-Fi first"); sec_end(r, 1, "NOT ON WI-FI"); return; }
    ensure_wifi_scan(r);
    if (g.ap_count == 0) { sec_line(r, "No networks found"); sec_end(r, 1, "NO WI-FI"); return; }
    int same = 0, openclone = 0, worst = 0;
    for (int i = 0; i < g.ap_count; i++) {
        if (strcmp(g.aps[i].ssid, g.w_ssid)) continue;
        if (!strcmp(g.aps[i].bssid, g.w_bssid)) continue;   // our own AP
        same++;
        if (!strcmp(g.aps[i].auth, "OPEN")) { openclone++; worst = 2; }
    }
    r->done = 80;
    sec_line(r, "SSID: %s", g.w_ssid);
    if (openclone) sec_line(r, "%d OPEN copy of your SSID!", openclone);
    if (same) { sec_line(r, "%d other radio%s share it", same, same == 1 ? "" : "s"); if (worst < 1) worst = 1; }
    else sec_line(r, "Only your AP broadcasts it");
    sec_line(r, same ? "Mesh is normal - verify each BSSID" : "No evil twin seen");
    if (worst == 2) sec_end(r, 2, "OPEN TWIN!");
    else if (worst == 1) sec_end(r, 1, "CHECK BSSIDS");
    else sec_end(r, 0, "NO TWIN");
    evlog("Rogue AP: %s (%d sharing SSID)", r->verdict, same);
}

// --- SNMP default community: does our gear answer to public/private (read-only)
static int snmp_get_sysdescr(uint32_t ip, const char *community, bool eth)
{
    uint8_t clen = strlen(community);
    // SNMPv2c GET sysDescr.0 (1.3.6.1.2.1.1.1.0)
    uint8_t pkt[64]; int n = 0;
    uint8_t oid[] = { 0x2b, 6, 1, 2, 1, 1, 1, 0 };
    int varbind = 2 + sizeof(oid) + 2;          // OID TLV + NULL TLV
    int vblist = 2 + varbind;
    int pdu = 2 + 1 + 2 + 1 + 2 + 1 + 2 + vblist; // req-id,err,erridx ints + vbl
    int msg = 2 + 1 + 2 + clen + 2 + pdu;         // version int + community + pdu
    pkt[n++] = 0x30; pkt[n++] = msg;
    pkt[n++] = 0x02; pkt[n++] = 0x01; pkt[n++] = 0x01;                 // version = 1 (v2c)
    pkt[n++] = 0x04; pkt[n++] = clen; memcpy(pkt + n, community, clen); n += clen;
    pkt[n++] = 0xA0; pkt[n++] = pdu;                                   // GET PDU
    pkt[n++] = 0x02; pkt[n++] = 0x01; pkt[n++] = 0x27;                 // request-id
    pkt[n++] = 0x02; pkt[n++] = 0x01; pkt[n++] = 0x00;                 // error
    pkt[n++] = 0x02; pkt[n++] = 0x01; pkt[n++] = 0x00;                 // error-index
    pkt[n++] = 0x30; pkt[n++] = vblist;
    pkt[n++] = 0x30; pkt[n++] = varbind;
    pkt[n++] = 0x06; pkt[n++] = sizeof(oid); memcpy(pkt + n, oid, sizeof(oid)); n += sizeof(oid);
    pkt[n++] = 0x05; pkt[n++] = 0x00;                                  // NULL value
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return -1;
    if (eth) bind_eth(s);
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(161), .sin_addr.s_addr = ip };
    sendto(s, pkt, n, 0, (struct sockaddr *)&to, sizeof(to));
    uint8_t rb[256];
    int rl = recv(s, rb, sizeof(rb), 0);
    close(s);
    return rl > 0 ? 1 : 0;
}
static void sec_snmp_scan(void)
{
    struct secres *r = &g.sec_snmp;
    sec_begin(r);
    const char *ip = g.eth_ip ? (g.sw_mgmt[0] ? g.sw_mgmt : g.gw) : (g.wifi_conn ? g.w_gw : NULL);
    struct in_addr a;
    if (!ip || !ip[0] || !resolve(ip, &a)) { sec_line(r, "No target"); sec_end(r, 1, "NO TARGET"); return; }
    bool eth = g.eth_ip;
    sec_line(r, "Target %s", ip);
    r->done = 30;
    bool pub = snmp_get_sysdescr(a.s_addr, "public", eth) == 1;
    r->done = 70;
    bool priv = snmp_get_sysdescr(a.s_addr, "private", eth) == 1;
    if (pub) sec_line(r, "Answers to 'public' (read)");
    if (priv) sec_line(r, "Answers to 'private'");
    if (pub || priv) {
        sec_line(r, "Change SNMP communities / use v3");
        sec_end(r, 2, "DEFAULT SNMP");
    } else {
        sec_line(r, "No reply to default communities");
        sec_end(r, 0, "OK / SNMP OFF");
    }
    evlog("SNMP check: %s", r->verdict);
}

// --- UPnP / SSDP: devices that announce themselves (and may open ports outward)
static void sec_upnp_scan(void)
{
    struct secres *r = &g.sec_upnp;
    sec_begin(r);
    bool eth = g.eth_ip;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { sec_line(r, "socket error"); sec_end(r, 1, "ERROR"); return; }
    if (eth) bind_eth(s);
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const char *m = "M-SEARCH * HTTP/1.1\r\nHOST:239.255.255.250:1900\r\nMAN:\"ssdp:discover\"\r\nMX:1\r\nST:ssdp:all\r\n\r\n";
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(1900) };
    inet_aton("239.255.255.250", (struct in_addr *)&to.sin_addr);
    sendto(s, m, strlen(m), 0, (struct sockaddr *)&to, sizeof(to));
    char buf[512];
    char ips[4][16]; int nips = 0;
    int64_t until = us() + 3000000;
    while (us() < until) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int rl = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        r->done = 30 + (int)((us() - (until - 3000000)) / 30000);
        if (rl <= 0) continue;
        char *fip = inet_ntoa(from.sin_addr);
        bool known = false;
        for (int i = 0; i < nips; i++) if (!strcmp(ips[i], fip)) known = true;
        if (!known && nips < 4) { snprintf(ips[nips], 16, "%s", fip); nips++; }
    }
    close(s);
    for (int i = 0; i < nips; i++) sec_line(r, "%s", ips[i]);
    if (nips) {
        sec_line(r, "%d UPnP device%s answered", nips, nips == 1 ? "" : "s");
        sec_line(r, "Disable UPnP on router if unused");
        sec_end(r, 1, "UPnP ACTIVE");
    } else { sec_line(r, "No UPnP/SSDP replies"); sec_end(r, 0, "QUIET"); }
    evlog("UPnP check: %s (%d devices)", r->verdict, nips);
}

// --- TLS / certificate check on an HTTPS service (switch mgmt or gateway by default)
static void sec_tls_check(const char *arg)
{
    struct secres *r = &g.sec_tls;
    sec_begin(r);
    char host[64];
    snprintf(host, sizeof(host), "%s", (arg && arg[0]) ? arg : (g.sw_mgmt[0] ? g.sw_mgmt : (g.eth_ip ? g.gw : g.w_gw)));
    int port = 443;
    char *c = strrchr(host, ':');
    if (c) { *c = 0; port = atoi(c + 1); }
    if (!host[0]) { sec_line(r, "No host"); sec_end(r, 1, "NO HOST"); return; }
    sec_line(r, "%s:%d", host, port);
    r->done = 20;
    // trusted handshake first
    esp_tls_cfg_t cfg = { .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 6000 };
    esp_tls_t *t = esp_tls_init();
    int worst = 0;
    bool trusted = t && esp_tls_conn_new_sync(host, strlen(host), port, &cfg, t) == 1;
    r->done = 55;
    if (!trusted) {
        if (t) { esp_tls_conn_destroy(t); }
        // retry without verification to read the version/cert even if untrusted
        esp_tls_cfg_t ncfg = { .skip_common_name = true, .timeout_ms = 6000 };
        t = esp_tls_init();
        bool any = t && esp_tls_conn_new_sync(host, strlen(host), port, &ncfg, t) == 1;
        r->done = 80;
        if (!any) { if (t) esp_tls_conn_destroy(t); sec_line(r, "No TLS on this port"); sec_end(r, 1, "NO TLS"); return; }
        sec_line(r, "Cert NOT trusted (self-signed/old)");
        worst = 2;
    } else sec_line(r, "Cert trusted by CA bundle");
    // TLS version + cert expiry via mbedTLS
    mbedtls_ssl_context *ssl = esp_tls_get_ssl_context(t);
    if (ssl) {
        const char *ver = mbedtls_ssl_get_version(ssl);
        sec_line(r, "Protocol %s", ver ? ver : "?");
        if (ver && (strstr(ver, "1.0") || strstr(ver, "1.1"))) worst = worst < 2 ? 2 : worst;
        const mbedtls_x509_crt *crt = mbedtls_ssl_get_peer_cert(ssl);
        if (crt) {
            const mbedtls_x509_time *v = &crt->valid_to;
            sec_line(r, "Expires %04d-%02d-%02d", v->year, v->mon, v->day);
        }
    }
    esp_tls_conn_destroy(t);
    r->done = 100;
    if (worst == 2) sec_end(r, 2, "WEAK TLS");
    else sec_end(r, 0, "TLS OK");
    evlog("TLS check %s: %s", host, r->verdict);
}

// --- DHCP watch: how many DHCP servers are handing out leases on this wire
static void sec_dhcp_scan(void)
{
    struct secres *r = &g.sec_dhcp;
    sec_begin(r);
    r->done = 100;
    if (g.dhcp_seen_n <= 0) { sec_line(r, g.eth_ip ? "Our server: %s" : "No DHCP seen yet", g.dhcp_srv);
        sec_line(r, "Listening passively on the wire");
        sec_end(r, 0, g.eth_ip ? "ONE SERVER" : "WATCHING"); }
    else {
        for (int i = 0; i < g.dhcp_seen_n && i < 4; i++) sec_line(r, "%s", g.dhcp_seen[i]);
        if (g.dhcp_seen_n > 1) { sec_line(r, "Rogue DHCP! more than one server"); sec_end(r, 2, "ROGUE DHCP"); }
        else { sec_line(r, "Single DHCP server"); sec_end(r, 0, "ONE SERVER"); }
    }
    evlog("DHCP watch: %s (%d servers)", r->verdict, g.dhcp_seen_n);
}

// --- Segment / guest isolation: can we reach something we should NOT from this port
static void sec_seg_check(const char *arg)
{
    struct secres *r = &g.sec_seg;
    sec_begin(r);
    // default probe target: the switch management IP (often on a separate mgmt VLAN)
    char tgt[32];
    snprintf(tgt, sizeof(tgt), "%s", (arg && arg[0]) ? arg : g.sw_mgmt);
    if (!tgt[0]) { sec_line(r, "No mgmt IP known"); sec_line(r, "Enter a target IP in the GUI"); sec_end(r, 1, "NO TARGET"); return; }
    struct in_addr a;
    if (!resolve(tgt, &a)) { sec_line(r, "Bad target"); sec_end(r, 1, "BAD TARGET"); return; }
    sec_line(r, "Probing %s", tgt);
    bool eth = g.eth_ip;
    r->done = 30;
    pingres_t pr = ping_run(a.s_addr, 3, eth, NULL);
    r->done = 70;
    bool web = tcp_probe(a.s_addr, 443, eth, 800) >= 0 || tcp_probe(a.s_addr, 80, eth, 800) >= 0;
    if (pr.recv || web) {
        sec_line(r, "Reachable from this port");
        sec_line(r, "Mgmt should be isolated by VLAN");
        sec_end(r, 2, "REACHABLE");
    } else { sec_line(r, "Not reachable (good)"); sec_end(r, 0, "ISOLATED"); }
    evlog("Segment test %s: %s", tgt, r->verdict);
}

// --- Bufferbloat: added latency to the gateway while the link is saturated
static volatile bool s_load_run;
static void load_task(void *a)
{
    (void)a;
    char *buf = malloc(8192);
    if (buf) {
        esp_http_client_config_t c = { .url = "https://speed.cloudflare.com/__down?bytes=50000000",
            .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 4000, .buffer_size = 4096 };
        esp_http_client_handle_t h = esp_http_client_init(&c);
        if (h && esp_http_client_open(h, 0) == ESP_OK && esp_http_client_fetch_headers(h) >= 0)
            while (s_load_run && esp_http_client_read(h, buf, 8192) > 0) {}
        if (h) esp_http_client_cleanup(h);
        free(buf);
    }
    s_load_run = false;
    vTaskDelete(NULL);
}
static void sec_buffer_test(void)
{
    struct secres *r = &g.sec_buffer;
    sec_begin(r);
    const char *g4 = g.eth_ip ? g.gw : (g.wifi_conn ? g.w_gw : NULL);
    struct in_addr gw;
    if (!g4 || !g4[0] || !resolve(g4, &gw)) { sec_line(r, "No gateway"); sec_end(r, 1, "NO GW"); return; }
    bool eth = g.eth_ip;
    sec_line(r, "Baseline ping...");
    r->done = 15;
    pingres_t base = ping_run(gw.s_addr, 8, eth, NULL);
    int idle = base.recv ? base.sum / base.recv : 0;
    // saturate the link and ping under load
    s_load_run = true;
    xTaskCreate(load_task, "bbload", 6144, NULL, 3, NULL);
    vTaskDelay(pdMS_TO_TICKS(800));
    r->done = 45;
    pingres_t load = ping_run(gw.s_addr, 12, eth, NULL);
    s_load_run = false;
    int busy = load.recv ? load.sum / load.recv : 0;
    int added = busy - idle; if (added < 0) added = 0;
    r->done = 90;
    sec_line(r, "Idle %d ms  loaded %d ms", idle, busy);
    sec_line(r, "Added under load: %d ms", added);
    int lvl; const char *grade;
    if (added < 30) { lvl = 0; grade = "A - EXCELLENT"; }
    else if (added < 100) { lvl = 0; grade = "B - GOOD"; }
    else if (added < 250) { lvl = 1; grade = "C - FAIR"; }
    else { lvl = 2; grade = "D - BLOATED"; }
    sec_line(r, "Grade %s", grade);
    sec_end(r, lvl, grade);
    evlog("Bufferbloat: +%d ms under load (%s)", added, grade);
}

static void tool_scan(const char *host)
{
    static const struct { int p; const char *n; } ports[] = {
        {21, "FTP"}, {22, "SSH"}, {23, "Telnet"}, {25, "SMTP"}, {53, "DNS"}, {80, "HTTP"}, {135, "MS-RPC"},
        {139, "NetBIOS"}, {443, "HTTPS"}, {445, "SMB"}, {515, "LPD"}, {554, "RTSP"}, {631, "IPP"},
        {1433, "MSSQL"}, {3306, "MySQL"}, {3389, "RDP"}, {5060, "SIP"}, {5900, "VNC"}, {8080, "HTTP-alt"},
        {8443, "HTTPS-alt"}, {9100, "Printer"},
    };
    struct in_addr a;
    if (!resolve(host, &a)) { tlog("cannot resolve %s", host); return; }
    tlog("PORT SCAN %s (%d common ports)", inet_ntoa(a), (int)(sizeof(ports) / sizeof(ports[0])));
    int open = 0;
    for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
        int ms = tcp_probe(a.s_addr, ports[i].p, false, 600);
        if (ms >= 0) { tlog("  %5d  %-9s open (%d ms)", ports[i].p, ports[i].n, ms); open++; }
    }
    tlog("%d open", open);
}

// ---------------------------------------------------------------------
// Wake-on-LAN magic packet
// ---------------------------------------------------------------------
static void tool_wol(const char *mac)
{
    unsigned m[6];
    if (sscanf(mac, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6 &&
        sscanf(mac, "%x-%x-%x-%x-%x-%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        tlog("enter a MAC like AA:BB:CC:DD:EE:FF");
        return;
    }
    uint8_t pkt[102];
    memset(pkt, 0xFF, 6);
    for (int i = 0; i < 16; i++) for (int k = 0; k < 6; k++) pkt[6 + i * 6 + k] = m[k];
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { tlog("socket error"); return; }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    bind_eth(s);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(9), .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
    for (int i = 0; i < 3; i++) sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to));
    close(s);
    tlog("Wake-on-LAN sent to %s (x3, UDP 9 broadcast)", mac);
}

static void tool_task(void *a)
{
    (void)a;
    if (!strcmp(s_type, "ping")) tool_ping(s_arg);
    else if (!strcmp(s_type, "trace")) tool_trace(s_arg);
    else if (!strcmp(s_type, "dns")) tool_dns(s_arg);
    else if (!strcmp(s_type, "port")) tool_port(s_arg);
    else if (!strcmp(s_type, "sweep")) tool_sweep();
    else if (!strcmp(s_type, "speed")) tool_speed(!g.eth_ip);
    else if (!strcmp(s_type, "wspeed")) tool_speed(true);
    else if (!strcmp(s_type, "espeed")) tool_speed(false);
    else if (!strcmp(s_type, "scan")) tool_scan(s_arg);
    else if (!strcmp(s_type, "wol")) tool_wol(s_arg);
    else if (!strcmp(s_type, "secrouter")) { sec_router_scan(); sec_report(&g.sec_router, "ROUTER SCAN"); }
    else if (!strcmp(s_type, "secwifi")) { sec_wifi_scan(); sec_report(&g.sec_wifi, "WI-FI AUDIT"); }
    else if (!strcmp(s_type, "secdns")) { sec_dns_scan(); sec_report(&g.sec_dns, "DNS CHECK"); }
    else if (!strcmp(s_type, "secarp")) { sec_arp_selftest(); sec_report(&g.sec_arp, "ARP TEST"); }
    else if (!strcmp(s_type, "secrogue")) { sec_rogue_scan(); sec_report(&g.sec_rogue, "ROGUE AP"); }
    else if (!strcmp(s_type, "secsnmp")) { sec_snmp_scan(); sec_report(&g.sec_snmp, "SNMP CHECK"); }
    else if (!strcmp(s_type, "secupnp")) { sec_upnp_scan(); sec_report(&g.sec_upnp, "UPnP SCAN"); }
    else if (!strcmp(s_type, "sectls")) { sec_tls_check(s_arg); sec_report(&g.sec_tls, "TLS CHECK"); }
    else if (!strcmp(s_type, "secdhcp")) { sec_dhcp_scan(); sec_report(&g.sec_dhcp, "DHCP WATCH"); }
    else if (!strcmp(s_type, "secseg")) { sec_seg_check(s_arg); sec_report(&g.sec_seg, "SEGMENT"); }
    else if (!strcmp(s_type, "buffer")) { sec_buffer_test(); sec_report(&g.sec_buffer, "BUFFERBLOAT"); }
    else tlog("unknown tool");
    tlog("");
    g.tool_running = false;
    vTaskDelete(NULL);
}

void tool_start(const char *type, const char *arg)
{
    bst_lock();
    if (g.tool_running) { bst_unlock(); return; }
    snprintf(s_type, sizeof(s_type), "%s", type ? type : "");
    snprintf(s_arg, sizeof(s_arg), "%s", arg ? arg : "");
    if (!strcmp(s_type, "quality")) { bst_unlock(); quality_start(); return; }
    static const char *noarg[] = { "sweep","speed","wspeed","espeed","secrouter","secwifi","secdns","secarp",
        "secrogue","secsnmp","secupnp","sectls","secdhcp","secseg","buffer" };
    bool need_arg = s_arg[0] == 0;
    for (size_t i = 0; need_arg && i < sizeof(noarg)/sizeof(noarg[0]); i++) if (!strcmp(s_type, noarg[i])) need_arg = false;
    if (need_arg) { tlog("enter a host first"); bst_unlock(); return; }
    g.tool_running = true;
    bst_unlock();
    if (xTaskCreate(tool_task, "tool", 8192, NULL, 3, NULL) != pdPASS) g.tool_running = false;
}
