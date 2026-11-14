

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_eth.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "buster.h"
#include "board.h"

static const char *TAG = "eth";

esp_netif_t *g_eth_netif = NULL;
static esp_eth_handle_t s_eth = NULL;
static int64_t s_link_t0 = 0;

#define W5500_PHYCFGR  (0x002E << 16)




static void scopy(char *dst, size_t cap, const uint8_t *src, int len)
{
    if (len < 0) len = 0;
    if ((size_t)len > cap - 1) len = cap - 1;
    int o = 0;
    for (int i = 0; i < len; i++) {
        uint8_t c = src[i];
        if (c == '\r' || c == '\n') { if (o) break; else continue; }
        dst[o++] = (c >= 32 && c < 127) ? c : '?';
    }
    dst[o] = 0;
}

static void fmt_mac(char *dst, size_t cap, const uint8_t *m)
{
    snprintf(dst, cap, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void caps_str(char *out, size_t cap, uint16_t bits, bool cdp)
{
    out[0] = 0;


    static const char *l[] = {"Other", "Repeater", "Switch", "AP", "Router", "Phone", "DOCSIS", "Station"};
    static const char *c[] = {"Router", "Bridge", "SRBridge", "Switch", "Host", "IGMP", "Repeater", "Phone"};
    for (int i = 0; i < 8; i++) {
        if (bits & (1 << i)) {
            if (cdp && i == 1 && (bits & 0x08)) continue;
            strncat(out, out[0] ? " " : "", cap - strlen(out) - 1);
            strncat(out, cdp ? c[i] : l[i], cap - strlen(out) - 1);
        }
    }
}

static void mau_str(char *out, size_t cap, uint16_t mau)
{
    const char *s = NULL;
    switch (mau) {
    case 10: s = "10M Half"; break;   case 11: s = "10M Full"; break;
    case 15: s = "100M Half"; break;  case 16: s = "100M Full"; break;
    case 29: s = "1G Half"; break;    case 30: s = "1G Full"; break;
    default: break;
    }
    if (s) snprintf(out, cap, "%s", s);
}

static void mark_found(const char *proto)
{
    bool was = g.sw_found;
    snprintf(g.sw_proto, sizeof(g.sw_proto), "%s", proto);
    g.sw_found = true;
    g.sw_seen_ms = now_ms();
    g.sw_frames++;
    if (!was) ui_post_event(EV_SWITCH);
}




static void parse_lldp(const uint8_t *p, int len)
{
    char name[48] = "", port[40] = "", desc[48] = "", sysd[96] = "", mgmt[16] = "", chassis[18] = "";
    char caps[32] = "", duplex[16] = "", poe[32] = "", model[48] = "", advs[24] = "";
    int pvid = -1, vlan_names = 0, first_vlan = -1, voice = -1, ttl = -1;
    bool got = false;
    poe_info_t power; poe_reset(&power);
    int i = 0;
    while (i + 2 <= len) {
        uint16_t hdr = (p[i] << 8) | p[i + 1];
        int type = hdr >> 9, tl = hdr & 0x1FF;
        i += 2;
        if (type == 0 || i + tl > len) break;
        const uint8_t *v = &p[i];
        if (type == 127) poe_lldp(&power, v, tl);
        switch (type) {
        case 1:
            if (tl == 7 && v[0] == 4) fmt_mac(chassis, sizeof(chassis), v + 1);
            break;
        case 2:
            if (tl == 7 && v[0] == 3) fmt_mac(port, sizeof(port), v + 1);
            else if (tl > 1) scopy(port, sizeof(port), v + 1, tl - 1);
            got = true;
            break;
        case 3:  if (tl >= 2) ttl = (v[0] << 8) | v[1]; break;
        case 4:  scopy(desc, sizeof(desc), v, tl); break;
        case 5:  scopy(name, sizeof(name), v, tl); got = true; break;
        case 6:  scopy(sysd, sizeof(sysd), v, tl); break;
        case 7:  if (tl >= 4) caps_str(caps, sizeof(caps), (v[2] << 8) | v[3], false); break;
        case 8:  if (tl >= 6 && v[1] == 1) snprintf(mgmt, sizeof(mgmt), "%d.%d.%d.%d", v[2], v[3], v[4], v[5]); break;
        case 127:
            if (tl < 4) break;
            if (v[0] == 0x00 && v[1] == 0x80 && v[2] == 0xC2) {
                if (v[3] == 1 && tl >= 6) pvid = (v[4] << 8) | v[5];
                else if (v[3] == 3 && tl >= 7) { vlan_names++; if (first_vlan < 0) first_vlan = (v[4] << 8) | v[5]; }
            } else if (v[0] == 0x00 && v[1] == 0x12 && v[2] == 0x0F) {
                if (v[3] == 1 && tl >= 9) {
                    mau_str(duplex, sizeof(duplex), (v[7] << 8) | v[8]);
                    uint16_t adv = (v[5] << 8) | v[6];
                    bool s10 = adv & 0x6000, s100 = adv & 0x0F00, s1000 = adv & 0x0003;
                    snprintf(advs, sizeof(advs), "%s%s%s", s10 ? "10/" : "", s100 ? "100/" : "", s1000 ? "1000/" : "");
                    if (advs[0]) advs[strlen(advs) - 1] = 0;
                }
                else if (v[3] == 2 && tl >= 7 && !poe[0]) {
                    bool supported = v[4] & 0x02, enabled = v[4] & 0x04;
                    snprintf(poe, sizeof(poe), "%s class %d", enabled ? ((v[4] & 1) ? "PSE enabled" : "PD enabled") : (supported ? "PoE supported" : "no support"), v[6] ? v[6] - 1 : 0);
                }
            } else if (v[0] == 0x00 && v[1] == 0x12 && v[2] == 0xBB) {
                if (v[3] == 2 && tl >= 8 && v[4] == 1) {
                    uint32_t x = (v[5] << 16) | (v[6] << 8) | v[7];
                    if (!(x & 0x800000)) voice = (x >> 9) & 0xFFF;
                } else if (v[3] == 4 && tl >= 7) {
                    int dw = (v[5] << 8) | v[6];
                    snprintf(poe, sizeof(poe), "%d.%d W adv", dw / 10, dw % 10);
                } else if (v[3] == 10 && tl > 4) {
                    scopy(model, sizeof(model), v + 4, tl - 4);
                }
            }
            break;
        default: break;
        }
        i += tl;
    }
    if (!got) return;
    bst_lock();
    if (name[0])    snprintf(g.sw_name, sizeof(g.sw_name), "%s", name);
    else if (chassis[0]) snprintf(g.sw_name, sizeof(g.sw_name), "%s", chassis);
    snprintf(g.sw_port, sizeof(g.sw_port), "%s", port);
    snprintf(g.sw_pdesc, sizeof(g.sw_pdesc), "%s", desc);
    snprintf(g.sw_ver, sizeof(g.sw_ver), "%s", sysd);
    snprintf(g.sw_model, sizeof(g.sw_model), "%s", model[0] ? model : "");
    snprintf(g.sw_caps, sizeof(g.sw_caps), "%s", caps);
    snprintf(g.sw_mgmt, sizeof(g.sw_mgmt), "%s", mgmt);
    snprintf(g.sw_chassis, sizeof(g.sw_chassis), "%s", chassis);
    snprintf(g.sw_duplex, sizeof(g.sw_duplex), "%s", duplex);
    snprintf(g.sw_adv, sizeof(g.sw_adv), "%s", advs);
    snprintf(g.sw_poe, sizeof(g.sw_poe), "%s", poe);
    if (power.valid) { power.seen_ms = now_ms(); g.poe = power; }
    int show = pvid >= 0 ? pvid : first_vlan;
    if (show >= 0) snprintf(g.sw_vlan, sizeof(g.sw_vlan), "%d", show);
    if (voice > 0) snprintf(g.sw_voice, sizeof(g.sw_voice), "%d", voice);
    else g.sw_voice[0] = 0;
    if (vlan_names > 1)     snprintf(g.sw_mode, sizeof(g.sw_mode), "Trunk (%d)", vlan_names);
    else if (pvid >= 0)     snprintf(g.sw_mode, sizeof(g.sw_mode), "Access");
    else if (first_vlan >= 0) snprintf(g.sw_mode, sizeof(g.sw_mode), "Tagged");
    g.sw_vlan_count = vlan_names;
    g.sw_ttl = ttl;
    mark_found("LLDP");
    bst_unlock();
    ESP_LOGI(TAG, "LLDP %s port=%s vlan=%s voice=%d", g.sw_name, port, g.sw_vlan, voice);
}




static void cdp_addr(const uint8_t *v, int vl, char *out, size_t cap)
{

    if (vl < 4) return;
    int n = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
    int i = 4;
    while (n-- > 0 && i + 2 <= vl) {
        int plen = v[i + 1];
        int pi = i + 2;
        if (pi + plen + 2 > vl) return;
        int alen = (v[pi + plen] << 8) | v[pi + plen + 1];
        const uint8_t *a = &v[pi + plen + 2];
        if (pi + plen + 2 + alen > vl) return;
        if (plen == 1 && v[pi] == 0xCC && alen == 4) {
            snprintf(out, cap, "%d.%d.%d.%d", a[0], a[1], a[2], a[3]);
            return;
        }
        i = pi + plen + 2 + alen;
    }
}

static void parse_cdp(const uint8_t *p, int len, const char *proto)
{
    if (len < 4) return;
    char dev[48] = "", port[40] = "", ver[96] = "", plat[48] = "", mgmt[16] = "", caps[32] = "", duplex[16] = "", poe[32] = "";
    int vlan = -1, voice = -1;
    bool got = false;
    poe_info_t power = g.poe;
    bool power_seen = false;
    int ttl = p[1];
    int i = 4;
    while (i + 4 <= len) {
        uint16_t type = (p[i] << 8) | p[i + 1];
        uint16_t tl = (p[i + 2] << 8) | p[i + 3];
        if (tl < 4 || i + tl > len) break;
        const uint8_t *v = &p[i + 4];
        int vl = tl - 4;
        if (poe_cdp(&power, type, v, vl)) power_seen = true;
        switch (type) {
        case 0x0001: scopy(dev, sizeof(dev), v, vl); got = true; break;
        case 0x0002: if (!mgmt[0]) cdp_addr(v, vl, mgmt, sizeof(mgmt)); break;
        case 0x0003: scopy(port, sizeof(port), v, vl); got = true; break;
        case 0x0004: if (vl >= 4) caps_str(caps, sizeof(caps), v[3], true); break;
        case 0x0005: scopy(ver, sizeof(ver), v, vl); break;
        case 0x0006: scopy(plat, sizeof(plat), v, vl); break;
        case 0x000A: if (vl >= 2) vlan = (v[0] << 8) | v[1]; break;
        case 0x000B: if (vl >= 1) snprintf(duplex, sizeof(duplex), "%s", v[0] ? "Full" : "Half"); break;
        case 0x000E: if (vl >= 3) voice = (v[1] << 8) | v[2]; break;
        case 0x0010: if (vl >= 2) { int mw = (v[0] << 8) | v[1]; snprintf(poe, sizeof(poe), "%d.%d W used adv", mw / 1000, (mw % 1000) / 100); } break;
        case 0x0016: cdp_addr(v, vl, mgmt, sizeof(mgmt)); break;
        case 0x001A: if (vl >= 8) {
                uint32_t av = ((uint32_t)v[4] << 24) | ((uint32_t)v[5] << 16) | ((uint32_t)v[6] << 8) | v[7];
                snprintf(poe, sizeof(poe), "%u.%u W avail adv", (unsigned)(av / 1000), (unsigned)((av % 1000) / 100));
            } break;
        default: break;
        }
        i += tl;
    }
    if (!got) return;
    bst_lock();
    snprintf(g.sw_name, sizeof(g.sw_name), "%s", dev);
    snprintf(g.sw_port, sizeof(g.sw_port), "%s", port);
    snprintf(g.sw_ver, sizeof(g.sw_ver), "%s", ver);
    snprintf(g.sw_model, sizeof(g.sw_model), "%s", plat);
    snprintf(g.sw_caps, sizeof(g.sw_caps), "%s", caps);
    snprintf(g.sw_duplex, sizeof(g.sw_duplex), "%s", duplex);
    snprintf(g.sw_poe, sizeof(g.sw_poe), "%s", poe);
    if (power_seen) { power.seen_ms = now_ms(); g.poe = power; }
    snprintf(g.sw_mgmt, sizeof(g.sw_mgmt), "%s", mgmt);
    g.sw_pdesc[0] = 0;
    g.sw_chassis[0] = 0;
    if (vlan >= 0) {
        snprintf(g.sw_vlan, sizeof(g.sw_vlan), "%d", vlan);
        snprintf(g.sw_mode, sizeof(g.sw_mode), "Native VLAN");
    }
    if (voice > 0) snprintf(g.sw_voice, sizeof(g.sw_voice), "%d", voice);
    else g.sw_voice[0] = 0;
    g.sw_ttl = ttl;
    mark_found(proto);
    bst_unlock();
    ESP_LOGI(TAG, "%s %s port=%s vlan=%d voice=%d", proto, dev, port, vlan, voice);
}




static void parse_bpdu(const uint8_t *p, int len)
{

    if (len < 3 + 35 || p[0] != 0x42 || p[1] != 0x42) return;
    const uint8_t *b = p + 3;
    if (b[0] != 0 || b[1] != 0) return;
    if (b[3] != 0x00 && b[3] != 0x02) return;
    int prio = (b[5] << 8) | b[6];
    char mac[18]; fmt_mac(mac, sizeof(mac), b + 7);
    bst_lock();
    snprintf(g.stp_root, sizeof(g.stp_root), "%d/%s", prio & 0xF000, mac);
    g.stp_cost = (b[13] << 24) | (b[14] << 16) | (b[15] << 8) | b[16];
    g.stp_tc = b[4] & 0x01;
    if (g.stp_tc) g.stp_tc_ms = now_ms();
    g.stp_seen = true;
    bst_unlock();
}




static uint32_t sec_frames, sec_bytes, sec_bc, sec_mc;

static esp_err_t eth_input_hook(esp_eth_handle_t eth, uint8_t *buf, uint32_t len, void *priv)
{
    (void)eth;
    if (len >= 14) {
        g.rx_frames++; g.rx_bytes += len;
        sec_frames++; sec_bytes += len;
        if (buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0xFF) { g.rx_bcast++; sec_bc++; }
        else if (buf[0] & 1) { g.rx_mcast++; sec_mc++; }
        uint16_t et = (buf[12] << 8) | buf[13];
        if (et == 0x0806) g.rx_arp++;
        if (et == 0x88CC) {
            parse_lldp(buf + 14, len - 14);
        } else if (et <= 1500 && len >= 17) {
            if (buf[14] == 0xAA && buf[15] == 0xAA && buf[16] == 0x03 && len >= 22) {
                uint32_t oui = (buf[17] << 16) | (buf[18] << 8) | buf[19];
                uint16_t pid = (buf[20] << 8) | buf[21];
                if (oui == 0x00000C && pid == 0x2000) parse_cdp(buf + 22, len - 22, "CDP");
                else if (oui == 0x00E052 && pid == 0x2000) parse_cdp(buf + 22, len - 22, "FDP");
            } else if (buf[14] == 0x42 && buf[15] == 0x42) {
                parse_bpdu(buf + 14, len - 14);
            }
        }
    }
    pkt_process(buf, len);
    return esp_netif_receive(g_eth_netif, buf, len, priv);
}


static void eth_stats_task(void *a)
{
    (void)a;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t f = sec_frames, by = sec_bytes, bc = sec_bc, mc = sec_mc;
        sec_frames = sec_bytes = sec_bc = sec_mc = 0;
        g.fps = f; g.bps = by; g.bc_fps = bc; g.mc_fps = mc;
        g.bc_pct = f ? bc * 100 / f : 0;
        g.mc_pct = f ? mc * 100 / f : 0;
        memmove(g.fps_hist, g.fps_hist + 1, sizeof(g.fps_hist) - sizeof(g.fps_hist[0]));
        g.fps_hist[47] = f > 65535 ? 65535 : f;

        if (g.eth_link && s_eth) {
            eth_speed_t sp; eth_duplex_t du;
            if (esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &sp) == ESP_OK) g.eth_speed = sp == ETH_SPEED_100M ? 100 : 10;
            if (esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &du) == ESP_OK) g.eth_full = du == ETH_DUPLEX_FULL;
        }

        if (g.sw_found) {
            int ttl = g.sw_ttl > 0 ? g.sw_ttl : 180;
            if (ttl < 180) ttl = 180;
            if (now_ms() - g.sw_seen_ms > (int64_t)ttl * 1000 + 10000) g.sw_found = false;
        }
        alerts_update();
    }
}




static void clear_switch(void)
{
    bst_lock();
    g.sw_found = false; g.stp_seen = false;
    poe_reset(&g.poe);
    g.sw_name[0] = g.sw_port[0] = g.sw_vlan[0] = g.sw_voice[0] = g.sw_mode[0] = 0;
    g.sw_mgmt[0] = g.sw_model[0] = g.sw_ver[0] = g.sw_caps[0] = g.sw_poe[0] = 0;
    g.sw_duplex[0] = g.sw_pdesc[0] = g.sw_chassis[0] = g.sw_adv[0] = 0;
    g.stp_tc_ms = 0;
    g.sw_frames = 0;
    bst_unlock();
}

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == ETHERNET_EVENT_CONNECTED) {
        g.eth_link = true;
        s_link_t0 = now_ms();
        g.link_up_ms = s_link_t0;
        if (!g.blinking) { clear_switch(); pkt_reset(); ui_post_event(EV_LINK_UP); evlog("Link up"); }
        if (cfg.static_ip) eth_apply_ip_config();
        ESP_LOGI(TAG, "link up");
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        g.eth_link = false; g.eth_ip = false; g.lease_s = 0;
        if (!g.blinking) {
            g.link_flaps++;
            memmove(g.flap_ms + 1, g.flap_ms, sizeof(g.flap_ms) - sizeof(g.flap_ms[0]));
            g.flap_ms[0] = now_ms();
            ui_post_event(EV_LINK_DOWN);
            evlog("Link down");
        }
        ESP_LOGI(TAG, "link down");
    }
}

static void got_ip_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    snprintf(g.ip, sizeof(g.ip), IPSTR, IP2STR(&e->ip_info.ip));
    snprintf(g.mask, sizeof(g.mask), IPSTR, IP2STR(&e->ip_info.netmask));
    snprintf(g.gw, sizeof(g.gw), IPSTR, IP2STR(&e->ip_info.gw));
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(g_eth_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK)
        snprintf(g.dns, sizeof(g.dns), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    g.dhcp_ms = cfg.static_ip ? 0 : (int)(now_ms() - s_link_t0);

    struct netif *nif = esp_netif_get_netif_impl(g_eth_netif);
    struct dhcp *d = (nif && !cfg.static_ip) ? netif_dhcp_data(nif) : NULL;
    if (cfg.static_ip) { snprintf(g.dhcp_srv, sizeof(g.dhcp_srv), "static"); g.lease_s = 0; }
    else if (d) {
        snprintf(g.dhcp_srv, sizeof(g.dhcp_srv), "%s", ipaddr_ntoa(&d->server_ip_addr));
        g.lease_s = d->offered_t0_lease;
    }
    g.eth_ip = true;
    ESP_LOGI(TAG, "IP %s gw %s dns %s (dhcp %d ms)", g.ip, g.gw, g.dns, g.dhcp_ms);
    evlog("Got IP %s (%s)", g.ip, cfg.static_ip ? "static" : "DHCP");
    if (!g.blinking && cfg.autotest) autotest_start();
}




static void phy_write(uint32_t v)
{
    esp_eth_phy_reg_rw_data_t rw = { .reg_addr = W5500_PHYCFGR, .reg_value_p = &v };
    esp_eth_ioctl(s_eth, ETH_CMD_WRITE_PHY_REG, &rw);
}

static void blink_task(void *arg)
{
    int secs = (int)(intptr_t)arg;
    ESP_LOGI(TAG, "port blink %ds", secs);
    for (int i = 0; i < secs; i++) {
        bool down = (i & 1) == 0;
        uint32_t mode = down ? 0x30 : 0x38;
        phy_write(0x40 | mode);
        vTaskDelay(pdMS_TO_TICKS(20));
        phy_write(0xC0 | mode);
        vTaskDelay(pdMS_TO_TICKS(980));
    }
    phy_write(0x40 | 0x38);
    vTaskDelay(pdMS_TO_TICKS(20));
    phy_write(0xC0 | 0x38);
    vTaskDelay(pdMS_TO_TICKS(4000));
    g.blinking = false;
    vTaskDelete(NULL);
}

void eth_blink_start(int seconds)
{
    if (g.blinking || !s_eth) return;
    if (seconds <= 0) seconds = cfg.blink_s;
    g.blinking = true;
    evlog("Port blink %ds", seconds);
    if (seconds < 4) seconds = 4;
    if (seconds > 60) seconds = 60;
    xTaskCreate(blink_task, "blink", 3072, (void *)(intptr_t)seconds, 4, NULL);
}




void eth_apply_ip_config(void)
{
    if (!g_eth_netif) return;
    esp_netif_ip_info_t ip = {0};
    if (cfg.static_ip && esp_netif_str_to_ip4(cfg.s_ip, &ip.ip) == ESP_OK && ip.ip.addr) {
        esp_netif_str_to_ip4(cfg.s_mask, &ip.netmask);
        esp_netif_str_to_ip4(cfg.s_gw, &ip.gw);
        esp_netif_dhcpc_stop(g_eth_netif);
        esp_netif_set_ip_info(g_eth_netif, &ip);
        esp_netif_dns_info_t dns = {0};
        if (esp_netif_str_to_ip4(cfg.s_dns, &dns.ip.u_addr.ip4) == ESP_OK && dns.ip.u_addr.ip4.addr) {
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            esp_netif_set_dns_info(g_eth_netif, ESP_NETIF_DNS_MAIN, &dns);
        }
        snprintf(g.ip, sizeof(g.ip), "%s", cfg.s_ip);
        snprintf(g.mask, sizeof(g.mask), "%s", cfg.s_mask);
        snprintf(g.gw, sizeof(g.gw), "%s", cfg.s_gw);
        snprintf(g.dns, sizeof(g.dns), "%s", cfg.s_dns);
        snprintf(g.dhcp_srv, sizeof(g.dhcp_srv), "static");
        g.lease_s = 0; g.dhcp_ms = 0;
        if (g.eth_link && !g.eth_ip) {
            g.eth_ip = true;
            if (cfg.autotest && !g.blinking) autotest_start();
        }
        ESP_LOGI(TAG, "static IP %s", cfg.s_ip);
    } else {
        esp_netif_dhcp_status_t st;
        if (esp_netif_dhcpc_get_status(g_eth_netif, &st) == ESP_OK && st != ESP_NETIF_DHCP_STARTED) {
            g.eth_ip = false;
            s_link_t0 = now_ms();
            esp_netif_dhcpc_start(g_eth_netif);
        }
    }
}

void eth_dhcp_renew(void)
{
    if (!g_eth_netif || cfg.static_ip) return;
    g.eth_ip = false;
    s_link_t0 = now_ms();
    esp_netif_dhcpc_stop(g_eth_netif);
    esp_netif_dhcpc_start(g_eth_netif);
    evlog("DHCP release/renew");
}


void eth_start(void)
{
    poe_reset(&g.poe);
    gpio_install_isr_service(0);
    gpio_config_t rst = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << ETH_RST };
    gpio_config(&rst);
    gpio_set_level(ETH_RST, 0); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(ETH_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));

    spi_bus_config_t b = { .mosi_io_num = ETH_MOSI, .miso_io_num = ETH_MISO, .sclk_io_num = ETH_CLK,
                           .quadwp_io_num = -1, .quadhd_io_num = -1 };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &b, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t d = { .mode = 0, .clock_speed_hz = 20 * 1000 * 1000,
                                        .spics_io_num = ETH_CS, .queue_size = 20 };
    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &d);
    w5500.int_gpio_num = ETH_INT;
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size = 4096;
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500, &mac_cfg);
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num = -1;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    if (esp_eth_driver_install(&eth_cfg, &s_eth) != ESP_OK) {
        ESP_LOGE(TAG, "W5500 not found - Ethernet disabled");
        evlog("Ethernet chip not found");
        s_eth = NULL;
        return;
    }
    g.eth_hw = true;
    bool promisc = true;
    esp_eth_ioctl(s_eth, ETH_CMD_S_PROMISCUOUS, &promisc);
    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_ETH);
    esp_eth_ioctl(s_eth, ETH_CMD_S_MAC_ADDR, m);
    fmt_mac(g.mac, sizeof(g.mac), m);


    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.route_prio = 200;
    esp_netif_config_t ncfg = { .base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    g_eth_netif = esp_netif_new(&ncfg);
    esp_netif_attach(g_eth_netif, esp_eth_new_netif_glue(s_eth));
    esp_eth_update_input_path(s_eth, eth_input_hook, NULL);
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_handler, NULL);
    esp_eth_start(s_eth);
    xTaskCreate(eth_stats_task, "eth_stats", 3072, NULL, 3, NULL);
}


bool poe_is_fresh(void)
{
    int ttl = g.sw_ttl > 0 ? g.sw_ttl : 120;
    return g.eth_link && g.sw_found && g.sw_ttl != 0 && g.poe.valid &&
           now_ms() - g.poe.seen_ms <= (int64_t)ttl * 1000;
}
