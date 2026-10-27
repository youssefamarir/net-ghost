// NetAlly Buster - shared state between the hardware tasks, the screen UI,
// the web GUI and the Bluetooth service.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "poe_decode.h"

#define FW_NAME     "Net Ghost"
#define FW_VERSION  "2.0.0"

// ---------------- settings (saved in NVS) ----------------
typedef struct {
    uint8_t  text_size;       // 0 standard, 1 large
    bool     contrast, animations;
    uint8_t  speed_mb;        // download payload limit 1..20 MB
    uint8_t  bright;          // backlight 5..100 %
    uint16_t sleep_s;         // screen off after N s idle, 0 = never
    bool     units_c;         // show Celsius instead of Fahrenheit
    bool     autotest;        // run AutoTest whenever the port gets an address
    bool     battery;         // battery fitted (reads BATT_ADC_GPIO)
    bool     hotspot;         // phone hotspot on
    uint8_t  blink_s;         // port blink length
    char     ap_pass[24];     // hotspot password
    bool     static_ip;       // Ethernet: static instead of DHCP
    char     s_ip[16], s_mask[16], s_gw[16], s_dns[16];
    char     tz[40];          // POSIX TZ string
    char     mqtt[64];        // hub broker URI ("" = off)
    char     quality_host[40];// target for the link quality test ("" = gateway)
    char     owm[40];         // OpenWeatherMap key ("" = weather off)
    bool     ble;             // Bluetooth on (advertising)
    bool     ble_pin;         // phones must pair with the PIN shown on screen
    char     ble_url[96];     // HTTPS page that hosts web/ble.html (Bluetooth GUI)
} cfg_t;

// ---------------- Wi-Fi scan ----------------
#define MAX_APS 24
typedef struct {
    char    ssid[33];
    char    bssid[18];
    int8_t  rssi;
    uint8_t chan;
    char    auth[8];
} ap_rec_t;

// ---------------- switch port configurator (SSH) ----------------
#define SW_MAX_VLANS 28
typedef struct {
    bool  busy, ok, connected;
    char  status[56];              // live status line
    char  host[40], user[24], port[40];  // last used (password never stored)
    int   vlans[SW_MAX_VLANS]; char vname[SW_MAX_VLANS][18]; int vlan_n;
    int   cur_vlan;                // the port's current untagged VLAN, if detected
    char  preview[160];            // exact CLI that apply will send
    char  result[400];            // raw switch output from the last action
} swcfg_t;

// ---------------- network device inventory (Find Hosts) ----------------
#define MAX_HOSTS 32
typedef struct { uint32_t ip; uint8_t mac[6]; char name[28]; } host_t;

// ---------------- AutoTest ----------------
typedef enum { T_IDLE = 0, T_RUN, T_PASS, T_WARN, T_FAIL } tstate_t;
enum { TS_LINK, TS_DHCP, TS_GW, TS_DNS, TS_NET, TS_SW, TS_COUNT };
typedef struct {
    tstate_t st;
    char     val[24];      // short result shown on screen ("3 ms", "100M FD")
    char     info[48];     // longer detail for the web GUI
} tstep_t;

// ---------------- alerts ----------------
#define MAX_ALERTS 10
typedef struct { uint8_t lvl; char txt[56]; } alert_t;   // lvl 0 info, 1 warn, 2 bad

// ---------------- packets ----------------
enum { PK_IPV4, PK_IPV6, PK_ARP, PK_LLDP, PK_CDP, PK_STP, PK_VLAN, PK_OTHER,
       PK_TCP, PK_UDP, PK_ICMP, PK_IGMP,
       PK_DHCP, PK_DNS, PK_MDNS, PK_SSDP, PK_NBNS, PK_LLMNR, PK_NTP, PK_SNMP,
       PK_COUNT };

typedef struct {
    bool running, down_ok, up_ok;
    uint8_t progress;
    float down_mbps, up_mbps;
    uint32_t down_bytes, up_bytes;
    int first_byte_ms;
    int64_t finished_ms;
    char stage[20], error[80], network[33], bssid[18];
    int channel, rssi;
} speed_result_t;

typedef struct {
    // ===== Ethernet port =====
    bool     eth_link, eth_ip, eth_hw;
    int      eth_speed;            // 10 / 100, 0 = unknown
    bool     eth_full;
    char     ip[16], mask[16], gw[16], dns[16], mac[18], dhcp_srv[16];
    uint32_t lease_s;
    int      dhcp_ms;              // link-up to IP
    int64_t  link_up_ms;           // when link came up (ms since boot)
    int      link_ms;              // power-on/plug to link (ms)
    uint32_t link_flaps;
    int64_t  flap_ms[4];           // last flap times

    // wire counters (everything the W5500 sees, promiscuous)
    uint32_t rx_frames, rx_bytes, rx_bcast, rx_mcast, rx_arp;
    uint32_t fps, bps, bc_fps, mc_fps;
    uint32_t bc_pct, mc_pct;
    uint16_t fps_hist[48];
    uint32_t pk[PK_COUNT];         // protocol counters

    // ===== switch (LLDP / CDP / FDP) =====
    bool     sw_found;
    char     sw_proto[6];
    char     sw_name[48];
    char     sw_port[40];
    char     sw_pdesc[48];
    char     sw_model[48];
    char     sw_ver[96];
    char     sw_caps[32];
    char     sw_vlan[8];
    char     sw_voice[8];
    char     sw_mode[20];
    char     sw_mgmt[16];
    char     sw_chassis[18];
    char     sw_duplex[16];        // switch side, e.g. "100M Full"
    char     sw_adv[24];           // speeds the switch port offers, e.g. "10/100/1000"
    char     sw_poe[32];
    int      sw_ttl;
    int      sw_vlan_count;
    int64_t  sw_seen_ms;
    uint32_t sw_frames;
    bool     stp_seen;
    char     stp_root[28];
    uint32_t stp_cost;
    bool     stp_tc;
    int64_t  stp_tc_ms;
    char     vlans_seen[32];       // 802.1Q tags seen on the wire

    poe_info_t poe;           // switch advertisements, never measured power

    // ===== link / cable quality =====
    bool     q_running;
    int      q_sent, q_recv, q_min, q_avg, q_max, q_jitter;   // ms
    char     q_target[16];
    int      q_done;               // progress 0..100

    // ===== Wi-Fi =====
    bool    wifi_conn;
    char    w_cfg_ssid[33];
    char    w_ssid[33], w_bssid[18], w_ip[16], w_gw[16], w_auth[8], w_phy[8];
    int     w_rssi, w_chan;
    char    w_mask[16], w_dns[48], w_mac[18], w_country[4];
    char    w_pairwise[16], w_group[16];
    int     w_freq, w_width, w_secondary, w_disconnect_reason, w_reconnects;
    int64_t w_connected_ms;
    bool    w_powersave;
    int8_t  rssi_hist[48];
    char    ap_ssid[33], ap_pass[24], ap_ip[16];
    int     ap_clients;
    ap_rec_t aps[MAX_APS];
    int      ap_count;
    uint8_t  chan_cnt[14];
    bool     scanning;
    int64_t  scan_ms;

    // ===== AutoTest =====
    tstep_t ts[TS_COUNT];
    bool    test_running;
    int     test_runs;
    int     test_result;           // 0 none, 1 pass, 2 warn, 3 fail

    // ===== alerts =====
    alert_t alerts[MAX_ALERTS];
    int     alert_n, alert_bad;
    char    dhcp_seen[2][16];      // DHCP servers heard on the wire
    int     dhcp_seen_n;
    char    ip_conflict[18];       // MAC that also claims our IP
    char    ra_router[18];         // IPv6 router advertisement source
    int     ra_count;
    char    igmp_querier[16];

    // ===== security scans (live results for the Tools pages) =====
    // one scan runs at a time (tool_running gates it); each page reads its own result
    struct secres { bool running; uint8_t done, level, n; char verdict[36]; char line[6][44]; }
        sec_router, sec_wifi, sec_dns, sec_arp,
        sec_rogue, sec_snmp, sec_upnp, sec_tls, sec_dhcp, sec_seg, sec_buffer;
    char gw_mac[18];          // gateway MAC learned from ARP; a change is a spoof signature
    bool gw_mac_changed;
    int  new_hosts;           // hosts seen that were not in the last sweep baseline

    // ===== tools =====
    bool     blinking;
    bool     tool_running;
    bool     sweep_running;
    int      sweep_found;
    int      sweep_done;
    host_t   hosts[MAX_HOSTS];
    int      host_n;
    float    speed_mbps;       // backward compatible Ethernet download result
    speed_result_t wifi_speed, eth_speedtest;

    // ===== battery / power =====
    bool     batt_present, batt_charging;
    float    batt_v;
    int      batt_pct;

    // ===== device =====
    float    chip_temp;
    uint32_t heap_free, heap_min;
    char     reset_reason[16];
    int64_t  boot_us;
    char     node_id[24];
    bool     screen_on;

    // ===== Bluetooth / links =====
    bool  ble_on, ble_conn;
    char  ble_name[24];
    bool  ble_secure;              // link is encrypted + authenticated (paired with PIN)
    bool  ble_pairing;             // PIN is on screen right now
    uint32_t ble_passkey;
    int   ble_bonds;               // phones remembered
    int   ble_mtu;
    uint32_t ble_api_reqs;         // GUI requests served over Bluetooth
    bool  mqtt_conn;

    // ===== weather (kept from SENTRY) =====
    bool  wx_valid;
    int   wx_temp;
    char  wx_cond[16];
} bst_t;

extern bst_t g;
extern cfg_t cfg;

void bst_lock(void);
void bst_unlock(void);

// events the UI shows as toasts
enum {
    EV_BLE_CONN     = 1 << 0,
    EV_BLE_DISC     = 1 << 1,
    EV_PHONE_AP     = 1 << 2,
    EV_SWITCH       = 1 << 3,
    EV_TEST_DONE    = 1 << 4,
    EV_LINK_UP      = 1 << 5,
    EV_LINK_DOWN    = 1 << 6,
    EV_ALERT        = 1 << 7,
    EV_BATT_LOW     = 1 << 8,
    EV_QUALITY_DONE = 1 << 9,
    EV_CFG          = 1 << 10,
    EV_BLE_PAIRED   = 1 << 11,
    EV_BLE_PAIRFAIL = 1 << 12,
};
void     ui_post_event(uint32_t ev);
uint32_t ui_take_events(void);

// ---------- actions (safe to call from any task) ----------
void autotest_start(void);
void quality_start(void);
void wifi_scan_start(void);
void eth_blink_start(int seconds);
void eth_dhcp_renew(void);
void eth_apply_ip_config(void);
void wifi_set_credentials(const char *ssid, const char *pass);
void wifi_hotspot_apply(void);
void tool_start(const char *type, const char *arg);
void sec_arp_selftest(void);   // emit a gratuitous ARP for our OWN address, then report
void tool_log_copy(char *out, size_t cap);
const char *tool_current(void);
int  hosts_json(char *out, size_t cap);
// switch configurator (SSH to a managed switch; authenticated, tech-driven)
void switch_cfg_connect(const char *host, const char *user, const char *pass, const char *port);
void switch_cfg_apply(const char *host, const char *user, const char *pass, const char *port, int vlan);
int  switch_cfg_json(char *out, size_t cap);
extern swcfg_t swcfg;
const char *mac_vendor(const uint8_t *mac);
void device_reboot(void);
void speed_cancel(void);
void speed_test_run(bool wifi);
bool poe_is_fresh(void);

// settings
void cfg_load(void);
void cfg_save(void);
enum { CFG_RESTART = 1, CFG_IP = 2, CFG_AP = 4, CFG_TZ = 8, CFG_BRIGHT = 16, CFG_BLE = 32 };
void ble_apply(void);                  // start/stop advertising after a settings change
void ble_forget_all(void);             // delete every paired phone
int  cfg_set(const char *key, const char *val);   // returns CFG_* flags for what changed
void cfg_apply(int flags);
int  cfg_json(char *out, size_t cap);
void cfg_factory_reset(void);
void display_set_brightness(int pct);

// event log
void evlog(const char *fmt, ...);
int  evlog_json(char *out, size_t cap);

// packets
void pkt_process(const uint8_t *buf, uint32_t len);
void pkt_reset(void);                  // new cable: forget the old network
void alerts_update(void);
void pkt_top_talker(char *out, size_t n);

// ---------- helpers ----------
int64_t now_ms(void);
const char *rssi_quality(int rssi);
float  c2f(float c);
void   json_esc(char *dst, size_t cap, const char *src);
int    build_status_json(char *out, size_t cap);
int    build_scan_json(char *out, size_t cap);
