// NetAlly Buster - packet analyzer and network alerts.
// Every frame the W5500 receives (promiscuous) passes through pkt_process():
// protocol counters, top talkers, rogue DHCP / IP conflict / IPv6 RA / IGMP querier
// detection, 802.1Q tags, and a rolling capture that downloads as a .pcap file.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "lwip/inet.h"
#include "buster.h"
#include "json.h"

static const char *TAG = "pkt";

// ---------------- rolling capture ----------------
#define CAP_N    64
#define CAP_SNAP 128
typedef struct { uint32_t s, us; uint16_t len, cap; uint8_t d[CAP_SNAP]; } capf_t;
static capf_t s_cap[CAP_N];
static int s_cap_head, s_cap_cnt;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// ---------------- talkers ----------------
#define TALK_N 24
typedef struct { uint8_t mac[6]; uint32_t ip; uint32_t frames, bytes, bc; int64_t last; } talker_t;
static talker_t s_talk[TALK_N];

static uint8_t  s_my_mac[6];
static uint32_t s_my_ip, s_gw_ip;   // network order
static uint8_t  s_ra_mac[2][6];
static uint16_t s_vlans[8];
static int      s_vlan_n;

static void mac_str(char *o, const uint8_t *m) { sprintf(o, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]); }
static void ip_str(char *o, const uint8_t *p) { sprintf(o, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]); }

static talker_t *talker(const uint8_t *mac)
{
    talker_t *free_t = NULL, *old = &s_talk[0];
    for (int i = 0; i < TALK_N; i++) {
        talker_t *t = &s_talk[i];
        if (t->frames && !memcmp(t->mac, mac, 6)) return t;
        if (!t->frames && !free_t) free_t = t;
        if (t->last < old->last) old = t;
    }
    talker_t *t = free_t ? free_t : old;
    memset(t, 0, sizeof(*t));
    memcpy(t->mac, mac, 6);
    return t;
}

static void note_dhcp_server(const uint8_t *ip)
{
    char s[16]; ip_str(s, ip);
    for (int i = 0; i < g.dhcp_seen_n; i++) if (!strcmp(g.dhcp_seen[i], s)) return;
    if (g.dhcp_seen_n < 2) {
        snprintf(g.dhcp_seen[g.dhcp_seen_n++], 16, "%s", s);
        evlog("DHCP server heard: %s", s);
    }
}

// DHCP message type (option 53) from a BOOTP payload
static int dhcp_type(const uint8_t *b, int len)
{
    if (len < 240 || b[236] != 99 || b[237] != 130 || b[238] != 83 || b[239] != 99) return 0;
    int i = 240;
    while (i + 1 < len && b[i] != 255) {
        if (b[i] == 0) { i++; continue; }
        if (b[i] == 53 && i + 2 < len) return b[i + 2];
        i += 2 + b[i + 1];
    }
    return 0;
}

void pkt_process(const uint8_t *buf, uint32_t len)
{
    if (len < 14) return;
    // rolling capture
    struct timeval tv; gettimeofday(&tv, NULL);
    portENTER_CRITICAL(&s_mux);
    capf_t *c = &s_cap[s_cap_head];
    c->s = tv.tv_sec; c->us = tv.tv_usec; c->len = len;
    c->cap = len > CAP_SNAP ? CAP_SNAP : len;
    memcpy(c->d, buf, c->cap);
    s_cap_head = (s_cap_head + 1) % CAP_N;
    if (s_cap_cnt < CAP_N) s_cap_cnt++;
    portEXIT_CRITICAL(&s_mux);

    const uint8_t *dst = buf, *src = buf + 6;
    bool bcast = dst[0] == 0xFF && dst[1] == 0xFF;
    talker_t *t = talker(src);
    t->frames++; t->bytes += len; t->last = now_ms();
    if (bcast) t->bc++;

    uint16_t et = (buf[12] << 8) | buf[13];
    int off = 14;
    if (et == 0x8100 && len >= 18) {
        g.pk[PK_VLAN]++;
        uint16_t vid = ((buf[14] << 8) | buf[15]) & 0x0FFF;
        bool have = false;
        for (int i = 0; i < s_vlan_n; i++) if (s_vlans[i] == vid) have = true;
        if (!have && s_vlan_n < 8) {
            s_vlans[s_vlan_n++] = vid;
            char b[32] = ""; size_t o = 0;
            for (int i = 0; i < s_vlan_n; i++) o += snprintf(b + o, sizeof(b) - o, "%s%u", i ? "," : "", s_vlans[i]);
            snprintf(g.vlans_seen, sizeof(g.vlans_seen), "%s", b);
            evlog("Tagged frames seen on VLAN %u", vid);
        }
        et = (buf[16] << 8) | buf[17];
        off = 18;
    }
    const uint8_t *p = buf + off;
    int pl = (int)len - off;

    if (et == 0x0800 && pl >= 20) {
        g.pk[PK_IPV4]++;
        int ihl = (p[0] & 0x0F) * 4;
        uint8_t proto = p[9];
        if (memcmp(p + 12, "\0\0\0\0", 4)) memcpy(&t->ip, p + 12, 4);
        const uint8_t *l4 = p + ihl;
        int l4len = pl - ihl;
        if (proto == 6) g.pk[PK_TCP]++;
        else if (proto == 1) g.pk[PK_ICMP]++;
        else if (proto == 2) {
            g.pk[PK_IGMP]++;
            if (l4len >= 1 && l4[0] == 0x11) ip_str(g.igmp_querier, p + 12);
        } else if (proto == 17 && l4len >= 8) {
            g.pk[PK_UDP]++;
            uint16_t sp = (l4[0] << 8) | l4[1], dp = (l4[2] << 8) | l4[3];
            uint16_t lo = sp < dp ? sp : dp;
            if (sp == 67 || dp == 67 || sp == 68 || dp == 68) {
                g.pk[PK_DHCP]++;
                if (sp == 67) {
                    int ty = dhcp_type(l4 + 8, l4len - 8);
                    if (ty == 2 || ty == 5) note_dhcp_server(p + 12);
                }
            } else if (sp == 53 || dp == 53) g.pk[PK_DNS]++;
            else if (sp == 5353 || dp == 5353) g.pk[PK_MDNS]++;
            else if (sp == 1900 || dp == 1900) g.pk[PK_SSDP]++;
            else if (lo == 137 || lo == 138) g.pk[PK_NBNS]++;
            else if (sp == 5355 || dp == 5355) g.pk[PK_LLMNR]++;
            else if (lo == 123) g.pk[PK_NTP]++;
            else if (lo == 161 || lo == 162) g.pk[PK_SNMP]++;
        }
    } else if (et == 0x86DD && pl >= 40) {
        g.pk[PK_IPV6]++;
        if (p[6] == 58 && pl >= 41 && p[40] == 134) {          // ICMPv6 router advertisement
            bool known = false;
            for (int i = 0; i < g.ra_count && i < 2; i++) if (!memcmp(s_ra_mac[i], src, 6)) known = true;
            if (!known && g.ra_count < 2) {
                memcpy(s_ra_mac[g.ra_count++], src, 6);
                mac_str(g.ra_router, src);
                evlog("IPv6 router advertisement from %s", g.ra_router);
            }
        }
        if (p[6] == 17 && pl >= 48) {
            uint16_t dp = (p[42] << 8) | p[43];
            if (dp == 5353) g.pk[PK_MDNS]++;
            else if (dp == 547 || dp == 546) g.pk[PK_DHCP]++;
            else if (dp == 1900) g.pk[PK_SSDP]++;
            else if (dp == 5355) g.pk[PK_LLMNR]++;
        }
    } else if (et == 0x0806 && pl >= 28) {
        g.pk[PK_ARP]++;
        uint32_t sip; memcpy(&sip, p + 14, 4);
        if (sip) t->ip = sip;
        // someone else answering for our address
        if (s_my_ip && sip == s_my_ip && memcmp(p + 8, s_my_mac, 6)) {
            if (!g.ip_conflict[0]) {
                mac_str(g.ip_conflict, p + 8);
                evlog("IP CONFLICT: %s also claims %s", g.ip_conflict, g.ip);
            }
        }
        // learn the gateway's MAC; a change to a different MAC is the classic ARP-spoof signature
        if (s_gw_ip && sip == s_gw_ip) {
            char m[18]; mac_str(m, p + 8);
            if (!g.gw_mac[0]) snprintf(g.gw_mac, sizeof(g.gw_mac), "%s", m);
            else if (strcmp(g.gw_mac, m) && !g.gw_mac_changed) {
                g.gw_mac_changed = true;
                evlog("GATEWAY MAC CHANGED: %s -> %s (possible ARP spoof)", g.gw_mac, m);
                snprintf(g.gw_mac, sizeof(g.gw_mac), "%s", m);
            }
        }
    } else if (et == 0x88CC) g.pk[PK_LLDP]++;
    else if (et <= 1500 && pl >= 3) {
        if (p[0] == 0xAA && p[1] == 0xAA && pl >= 8 && p[3] == 0 && p[4] == 0 && p[5] == 0x0C) g.pk[PK_CDP]++;
        else if (p[0] == 0x42 && p[1] == 0x42) g.pk[PK_STP]++;
        else g.pk[PK_OTHER]++;
    } else g.pk[PK_OTHER]++;
}

// ---------------------------------------------------------------------
// alerts
// ---------------------------------------------------------------------
static void add(int lvl, const char *fmt, ...)
{
    if (g.alert_n >= MAX_ALERTS) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(g.alerts[g.alert_n].txt, sizeof(g.alerts[0].txt), fmt, ap);
    va_end(ap);
    g.alerts[g.alert_n].lvl = lvl;
    g.alert_n++;
}

void alerts_update(void)
{
    static int last_bad = 0;
    // keep our own address handy for conflict detection
    struct in_addr a;
    s_my_ip = (g.eth_ip && inet_aton(g.ip, &a)) ? a.s_addr : 0;
    { struct in_addr gwa; s_gw_ip = (g.eth_ip && g.gw[0] && inet_aton(g.gw, &gwa)) ? gwa.s_addr : 0; }
    if (!s_my_mac[0] && !s_my_mac[1]) {
        unsigned m[6];
        if (sscanf(g.mac, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6)
            for (int i = 0; i < 6; i++) s_my_mac[i] = m[i];
    }
    int64_t t = now_ms();

    bst_lock();
    g.alert_n = 0;
    if (g.ip_conflict[0]) add(2, "IP conflict: %s also uses %s", g.ip_conflict, g.ip);
    if (g.gw_mac_changed) add(2, "Gateway MAC changed: %s", g.gw_mac);
    if (g.dhcp_seen_n > 1) add(2, "2 DHCP servers: %s + %s", g.dhcp_seen[0], g.dhcp_seen[1]);
    if (g.bc_fps > 300) add(2, "Broadcast storm: %u broadcasts/s", (unsigned)g.bc_fps);
    else if (g.bc_fps > 100) add(1, "High broadcast: %u/s", (unsigned)g.bc_fps);
    if (g.mc_fps > 1000) add(1, "Multicast flood: %u/s", (unsigned)g.mc_fps);
    if (g.eth_link && g.sw_duplex[0]) {
        bool sw_full = strstr(g.sw_duplex, "Full") != NULL, sw_half = strstr(g.sw_duplex, "Half") != NULL;
        if ((sw_full && !g.eth_full) || (sw_half && g.eth_full)) add(2, "Duplex mismatch: switch %s", g.sw_duplex);
    }
    if (g.eth_link && (g.eth_speed == 10 || !g.eth_full)) add(1, "Slow link: %dM %s duplex", g.eth_speed, g.eth_full ? "full" : "half");
    int recent = 0;
    for (int i = 0; i < 4; i++) if (g.flap_ms[i] && t - g.flap_ms[i] < 600000) recent++;
    if (recent >= 3) add(2, "Link flapping: %d drops in 10 min", recent);
    if (g.stp_tc_ms && t - g.stp_tc_ms < 300000) add(1, "Spanning tree topology change");
    if (g.test_result == 3) {
        for (int i = 0; i < TS_COUNT; i++) if (g.ts[i].st == T_FAIL) { add(2, "AutoTest failed: %s", g.ts[i].info); break; }
    }
    if (g.ra_count > 1) add(1, "2 IPv6 routers advertising");
    else if (g.ra_count == 1) add(0, "IPv6 router: %s", g.ra_router);
    if (g.eth_link && !g.sw_found && g.link_up_ms && t - g.link_up_ms > 90000) add(0, "Switch not announcing (LLDP/CDP off)");
    if (g.vlans_seen[0]) add(0, "Tagged VLANs on port: %s", g.vlans_seen);
    if (g.igmp_querier[0]) add(0, "IGMP querier: %s", g.igmp_querier);
    if (g.batt_present && g.batt_pct <= 15 && !g.batt_charging) add(1, "Battery low: %d%%", g.batt_pct);
    int bad = 0;
    for (int i = 0; i < g.alert_n; i++) if (g.alerts[i].lvl >= 1) bad++;
    g.alert_bad = bad;
    bst_unlock();
    if (bad > last_bad) ui_post_event(EV_ALERT);
    last_bad = bad;
}

// busiest sender, for the screen
void pkt_top_talker(char *out, size_t n)
{
    talker_t best = {0};
    uint32_t total = 0;
    bst_lock();
    for (int i = 0; i < TALK_N; i++) {
        total += s_talk[i].frames;
        if (s_talk[i].frames > best.frames && memcmp(s_talk[i].mac, s_my_mac, 6)) best = s_talk[i];
    }
    bst_unlock();
    if (!best.frames) { snprintf(out, n, "--"); return; }
    char who[18];
    if (best.ip) ip_str(who, (uint8_t *)&best.ip); else mac_str(who, best.mac);
    snprintf(out, n, "%s %u%%", who, (unsigned)(total ? best.frames * 100 / total : 0));
}

// forget what we learned about the old network when the cable moves
void pkt_reset(void)
{
    bst_lock();
    memset(s_talk, 0, sizeof(s_talk));
    memset(g.pk, 0, sizeof(g.pk));
    g.dhcp_seen_n = 0; g.ip_conflict[0] = 0; g.ra_count = 0; g.ra_router[0] = 0;
    g.gw_mac[0] = 0; g.gw_mac_changed = false; s_gw_ip = 0;
    g.igmp_querier[0] = 0; g.vlans_seen[0] = 0; s_vlan_n = 0;
    bst_unlock();
}

// ---------------------------------------------------------------------
// one-line decode of a captured frame (for the live packet list)
// ---------------------------------------------------------------------
static void summarize(const capf_t *f, char *src, char *dst, char *proto, char *info)
{
    const uint8_t *b = f->d;
    mac_str(src, b + 6);
    if (b[0] == 0xFF && b[1] == 0xFF) strcpy(dst, "broadcast"); else mac_str(dst, b);
    strcpy(proto, "ETH"); info[0] = 0;
    uint16_t et = (b[12] << 8) | b[13];
    int off = 14;
    char vtag[12] = "";
    if (et == 0x8100 && f->cap >= 18) { snprintf(vtag, sizeof(vtag), "[VLAN %u] ", ((b[14] << 8) | b[15]) & 0xFFF); et = (b[16] << 8) | b[17]; off = 18; }
    const uint8_t *p = b + off; int pl = f->cap - off;
    if (et == 0x0806 && pl >= 28) {
        strcpy(proto, "ARP");
        char s[16], t2[16]; ip_str(s, p + 14); ip_str(t2, p + 24);
        if (((p[6] << 8) | p[7]) == 1) snprintf(info, 72, "%sWho has %s? Tell %s", vtag, t2, s);
        else { char m[18]; mac_str(m, p + 8); snprintf(info, 72, "%s%s is at %s", vtag, s, m); }
    } else if (et == 0x0800 && pl >= 20) {
        int ihl = (p[0] & 0x0F) * 4;
        ip_str(src, p + 12); ip_str(dst, p + 16);
        const uint8_t *l4 = p + ihl; int l4l = pl - ihl;
        switch (p[9]) {
        case 1: strcpy(proto, "ICMP"); if (l4l > 0) snprintf(info, 72, "%s%s", vtag, l4[0] == 8 ? "Echo request" : l4[0] == 0 ? "Echo reply" : l4[0] == 3 ? "Unreachable" : l4[0] == 11 ? "TTL exceeded" : "ICMP"); break;
        case 2: strcpy(proto, "IGMP"); if (l4l > 0) snprintf(info, 72, "%s%s", vtag, l4[0] == 0x11 ? "Membership query" : "Membership report"); break;
        case 6: strcpy(proto, "TCP"); if (l4l >= 14) {
                static const char *fl = "FSRPAU"; char fs[8] = ""; int k = 0;
                for (int i = 0; i < 6; i++) if (l4[13] & (1 << i)) fs[k++] = fl[i];
                fs[k] = 0;
                snprintf(info, 72, "%s%u -> %u [%s]", vtag, (l4[0] << 8) | l4[1], (l4[2] << 8) | l4[3], fs);
            } break;
        case 17: if (l4l >= 8) {
                uint16_t sp = (l4[0] << 8) | l4[1], dp = (l4[2] << 8) | l4[3], lo = sp < dp ? sp : dp;
                const char *n = "UDP";
                if (lo == 67 || lo == 68) n = "DHCP"; else if (lo == 53) n = "DNS"; else if (lo == 5353) n = "mDNS";
                else if (lo == 1900) n = "SSDP"; else if (lo == 137 || lo == 138) n = "NetBIOS"; else if (lo == 5355) n = "LLMNR";
                else if (lo == 123) n = "NTP"; else if (lo == 161 || lo == 162) n = "SNMP";
                strcpy(proto, n);
                if (!strcmp(n, "DHCP")) {
                    static const char *dt[] = {"?", "Discover", "Offer", "Request", "Decline", "ACK", "NAK", "Release", "Inform"};
                    int ty = dhcp_type(l4 + 8, l4l - 8);
                    snprintf(info, 72, "%s%s", vtag, ty > 0 && ty < 9 ? dt[ty] : "(cut off)");
                } else snprintf(info, 72, "%s%u -> %u", vtag, sp, dp);
            } else strcpy(proto, "UDP");
            break;
        default: snprintf(proto, 8, "IP/%u", p[9]); break;
        }
    } else if (et == 0x86DD && pl >= 40) {
        strcpy(proto, "IPv6");
        if (p[6] == 58 && pl > 40) {
            strcpy(proto, "ICMPv6");
            uint8_t ty = p[40];
            snprintf(info, 72, "%s%s", vtag, ty == 134 ? "Router advertisement" : ty == 133 ? "Router solicitation" :
                     ty == 135 ? "Neighbor solicitation" : ty == 136 ? "Neighbor advertisement" : ty == 143 ? "MLD report" : "ICMPv6");
        } else if (p[6] == 17) strcpy(proto, "UDPv6");
        else if (p[6] == 6) strcpy(proto, "TCPv6");
    } else if (et == 0x88CC) { strcpy(proto, "LLDP"); snprintf(info, 72, "%sSwitch announcement", vtag); }
    else if (et == 0x888E) strcpy(proto, "EAPOL");
    else if (et <= 1500 && pl >= 3) {
        if (p[0] == 0xAA && pl >= 8 && p[5] == 0x0C) { strcpy(proto, "CDP"); snprintf(info, 72, "%sCisco discovery", vtag); }
        else if (p[0] == 0x42 && p[1] == 0x42) { strcpy(proto, "STP"); snprintf(info, 72, "%sBPDU", vtag); }
        else strcpy(proto, "LLC");
    } else snprintf(proto, 8, "%04X", et);
}

static int cap_snapshot(capf_t **out)
{
    capf_t *c = malloc(sizeof(s_cap));
    if (!c) return -1;
    portENTER_CRITICAL(&s_mux);
    int n = s_cap_cnt, head = s_cap_head;
    // oldest first
    for (int i = 0; i < n; i++) c[i] = s_cap[(head - n + i + CAP_N) % CAP_N];
    portEXIT_CRITICAL(&s_mux);
    *out = c;
    return n;
}

static void ago(char *o, size_t n, uint32_t s, uint32_t us)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    long ms = (long)(tv.tv_sec - s) * 1000 + ((long)tv.tv_usec - (long)us) / 1000;
    if (ms < 0) ms = 0;
    snprintf(o, n, "%ld.%01lds", ms / 1000, (ms % 1000) / 100);
}

// GET /api/packets
esp_err_t pkt_json_handler(httpd_req_t *req)
{
    static const char *names[PK_COUNT] = { "IPv4", "IPv6", "ARP", "LLDP", "CDP", "STP", "802.1Q", "Other",
        "TCP", "UDP", "ICMP", "IGMP", "DHCP", "DNS", "mDNS", "SSDP", "NetBIOS", "LLMNR", "NTP", "SNMP" };
    char *b = malloc(1600);
    if (!b) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    int o = snprintf(b, 1600, "{\"total\":%u,\"fps\":%u,\"pk\":{", (unsigned)g.rx_frames, (unsigned)g.fps);
    for (int i = 0; i < PK_COUNT; i++) o += snprintf(b + o, 1600 - o, "%s\"%s\":%u", i ? "," : "", names[i], (unsigned)g.pk[i]);
    o += snprintf(b + o, 1600 - o, "},\"talkers\":[");
    httpd_resp_sendstr_chunk(req, b);

    // top 10 talkers by frames
    talker_t top[TALK_N];
    bst_lock(); memcpy(top, s_talk, sizeof(top)); bst_unlock();
    for (int i = 0; i < TALK_N; i++) for (int j = i + 1; j < TALK_N; j++)
        if (top[j].frames > top[i].frames) { talker_t x = top[i]; top[i] = top[j]; top[j] = x; }
    int64_t t = now_ms();
    o = 0;
    for (int i = 0, k = 0; i < TALK_N && k < 10; i++) {
        if (!top[i].frames) break;
        char m[18], ip[16] = ""; mac_str(m, top[i].mac);
        if (top[i].ip) ip_str(ip, (uint8_t *)&top[i].ip);
        o += snprintf(b + o, 1600 - o, "%s{\"m\":\"%s\",\"ip\":\"%s\",\"f\":%u,\"b\":%u,\"bc\":%u,\"age\":%d,\"me\":%s}", k ? "," : "",
                      m, ip, (unsigned)top[i].frames, (unsigned)top[i].bytes, (unsigned)top[i].bc,
                      (int)((t - top[i].last) / 1000), memcmp(top[i].mac, s_my_mac, 6) ? "false" : "true");
        k++;
    }
    o += snprintf(b + o, 1600 - o, "],\"frames\":[");
    httpd_resp_sendstr_chunk(req, b);

    capf_t *c; int n = cap_snapshot(&c);
    for (int i = n - 1, k = 0; i >= 0 && k < 40; i--, k++) {
        char src[18], dst[18], proto[8], info[72], e[90], when[16];
        summarize(&c[i], src, dst, proto, info);
        json_esc(e, sizeof(e), info);
        ago(when, sizeof(when), c[i].s, c[i].us);
        snprintf(b, 1600, "%s{\"t\":\"%s\",\"l\":%u,\"s\":\"%s\",\"d\":\"%s\",\"p\":\"%s\",\"i\":\"%s\"}",
                 k ? "," : "", when, c[i].len, src, dst, proto, e);
        httpd_resp_sendstr_chunk(req, b);
    }
    if (n > 0) free(c);
    httpd_resp_sendstr_chunk(req, "]}");
    free(b);
    return httpd_resp_sendstr_chunk(req, NULL);
}

// GET /api/pcap - last 64 frames (first 128 bytes each) for Wireshark
esp_err_t pkt_pcap_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/vnd.tcpdump.pcap");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"net-ghost.pcap\"");
    struct __attribute__((packed)) { uint32_t magic; uint16_t vmaj, vmin; int32_t tz; uint32_t sig, snap, net; } gh =
        { 0xa1b2c3d4, 2, 4, 0, 0, CAP_SNAP, 1 };
    httpd_resp_send_chunk(req, (const char *)&gh, sizeof(gh));
    capf_t *c; int n = cap_snapshot(&c);
    for (int i = 0; i < n; i++) {
        struct __attribute__((packed)) { uint32_t s, us, incl, orig; } rh = { c[i].s, c[i].us, c[i].cap, c[i].len };
        httpd_resp_send_chunk(req, (const char *)&rh, sizeof(rh));
        httpd_resp_send_chunk(req, (const char *)c[i].d, c[i].cap);
    }
    if (n > 0) free(c);
    ESP_LOGI(TAG, "pcap download: %d frames", n);
    return httpd_resp_send_chunk(req, NULL, 0);
}
