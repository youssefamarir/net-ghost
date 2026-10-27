// Net Ghost - switch port VLAN configurator over SSH.
//
// This is authenticated switch administration: the technician supplies the switch IP and
// their own login in the GUI, Net Ghost opens an SSH shell, reads the VLAN list, and (only
// after the technician confirms the exact commands) moves the connected port to the chosen
// untagged/access VLAN. It never stores the password and never acts without a confirm.
//
// First target: Ruckus ICX (FastIron CLI). The command set is a small per-vendor template;
// other vendors slot in the same way.
//
// NOTE: validated on real hardware is still required. Prompts and output vary by firmware,
// so every raw line the switch returns is captured into swcfg.result for the tech to verify.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "libssh2.h"
#include "buster.h"

static const char *TAG = "swcfg";
static char *ci_str(const char *h, const char *n)
{
    size_t ln = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, ln)) return (char *)h;
    return NULL;
}
swcfg_t swcfg;

// one operation at a time
static char s_host[40], s_user[24], s_pass[64], s_port[40];
static int  s_vlan;

static void st(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(swcfg.status, sizeof(swcfg.status), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", swcfg.status);
}

static void append_result(const char *s, int n)
{
    int have = strlen(swcfg.result);
    for (int i = 0; i < n && have < (int)sizeof(swcfg.result) - 1; i++) {
        char c = s[i];
        if (c == '\r') continue;
        swcfg.result[have++] = c;
    }
    swcfg.result[have] = 0;
}

// ---------------------------------------------------------------------
// SSH shell helpers
// ---------------------------------------------------------------------
static int tcp_connect(const char *host, int port)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    char ps[8]; snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return -1;
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s >= 0) {
        struct timeval tv = { .tv_sec = 8, .tv_usec = 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(s, res->ai_addr, res->ai_addrlen) != 0) { close(s); s = -1; }
    }
    freeaddrinfo(res);
    return s;
}

// read from the shell for up to ms, copying into result; stop early once output goes idle
static void read_until_idle(LIBSSH2_CHANNEL *ch, int ms, char *capture, int cap)
{
    char buf[512];
    int64_t end = now_ms() + ms;
    int idle = 0, caplen = 0;
    if (capture && cap) capture[0] = 0;
    while (now_ms() < end) {
        int n = libssh2_channel_read(ch, buf, sizeof(buf));
        if (n > 0) {
            append_result(buf, n);
            if (capture) for (int i = 0; i < n && caplen < cap - 1; i++) capture[caplen++] = buf[i];
            idle = 0;
        } else if (n == LIBSSH2_ERROR_EAGAIN || n == 0) {
            if (++idle > 4) break;                 // ~400 ms of quiet = prompt is waiting
            vTaskDelay(pdMS_TO_TICKS(100));
        } else break;                              // channel closed / error
    }
    if (capture) capture[caplen] = 0;
}

static void send_cmd(LIBSSH2_CHANNEL *ch, const char *cmd, int wait_ms, char *capture, int cap)
{
    char line[160];
    int n = snprintf(line, sizeof(line), "%s\n", cmd);
    libssh2_channel_write(ch, line, n);
    read_until_idle(ch, wait_ms, capture, cap);
}

// Pull VLAN IDs out of "show vlan brief" style output: lines that start with a VLAN number.
static void parse_vlans(const char *out)
{
    swcfg.vlan_n = 0;
    const char *p = out;
    while (*p && swcfg.vlan_n < SW_MAX_VLANS) {
        // move to start of a line
        while (*p == ' ' || *p == '\t') p++;
        // FastIron: "VLAN <id>" or a leading integer column
        int id = -1; const char *q = p;
        if (!strncasecmp(p, "VLAN", 4)) { q = p + 4; while (*q == ' ') q++; }
        if (*q >= '0' && *q <= '9') {
            id = atoi(q);
            if (id >= 1 && id <= 4094) {
                bool dup = false;
                for (int i = 0; i < swcfg.vlan_n; i++) if (swcfg.vlans[i] == id) dup = true;
                if (!dup) {
                    swcfg.vlans[swcfg.vlan_n] = id;
                    swcfg.vname[swcfg.vlan_n][0] = 0;
                    swcfg.vlan_n++;
                }
            }
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
}

// open an SSH shell and log in; returns session+channel or NULL. caller must clean up.
static LIBSSH2_SESSION *ssh_login(int *sock_out, LIBSSH2_CHANNEL **ch_out)
{
    *ch_out = NULL;
    int sock = tcp_connect(s_host, 22);
    if (sock < 0) { st("Cannot reach %s:22", s_host); return NULL; }
    *sock_out = sock;
    if (libssh2_init(0)) { st("SSH init failed"); return NULL; }
    LIBSSH2_SESSION *sess = libssh2_session_init();
    if (!sess) { st("SSH session failed"); return NULL; }
    libssh2_session_set_blocking(sess, 1);
    libssh2_session_set_timeout(sess, 12000);
    if (libssh2_session_handshake(sess, sock)) { st("SSH handshake failed"); return sess; }
    if (libssh2_userauth_password(sess, s_user, s_pass)) { st("Login rejected - check user/pass"); return sess; }
    LIBSSH2_CHANNEL *ch = libssh2_channel_open_session(sess);
    if (!ch) { st("Channel open failed"); return sess; }
    libssh2_channel_request_pty(ch, "vt100");
    if (libssh2_channel_shell(ch)) { st("Shell request failed"); return sess; }
    read_until_idle(ch, 2500, NULL, 0);          // banner / first prompt
    // best-effort enable + no paging (Ruckus FastIron)
    char tmp[256];
    send_cmd(ch, "enable", 1500, tmp, sizeof(tmp));
    if (ci_str(tmp, "password")) send_cmd(ch, s_pass, 1500, NULL, 0);   // enable password = login pass, if asked
    send_cmd(ch, "skip-page-display", 1200, NULL, 0);
    send_cmd(ch, "terminal length 0", 1200, NULL, 0);                       // harmless if unsupported
    *ch_out = ch;
    return sess;
}

static void ssh_cleanup(int sock, LIBSSH2_SESSION *sess, LIBSSH2_CHANNEL *ch)
{
    if (ch) { libssh2_channel_send_eof(ch); libssh2_channel_free(ch); }
    if (sess) { libssh2_session_disconnect(sess, "bye"); libssh2_session_free(sess); }
    if (sock >= 0) close(sock);
    libssh2_exit();
}

// ---------------------------------------------------------------------
// tasks
// ---------------------------------------------------------------------
static void connect_task(void *a)
{
    (void)a;
    swcfg.result[0] = 0; swcfg.ok = false; swcfg.connected = false; swcfg.vlan_n = 0; swcfg.cur_vlan = 0;
    st("Connecting to %s...", s_host);
    int sock = -1; LIBSSH2_CHANNEL *ch = NULL;
    LIBSSH2_SESSION *sess = ssh_login(&sock, &ch);
    if (ch) {
        swcfg.connected = true;
        st("Reading VLANs...");
        char out[1400];
        send_cmd(ch, "show vlan brief", 3500, out, sizeof(out));
        if (strlen(out) < 20) send_cmd(ch, "show vlan", 3500, out, sizeof(out));
        parse_vlans(out);
        if (s_port[0]) {
            char q[64], pout[400];
            snprintf(q, sizeof(q), "show interfaces ethernet %s | include VLAN", s_port);
            send_cmd(ch, q, 2500, pout, sizeof(pout));
            char *v = ci_str(pout, "VLAN");
            if (v) { while (*v && (*v < '0' || *v > '9')) v++; if (*v) swcfg.cur_vlan = atoi(v); }
        }
        st(swcfg.vlan_n ? "Connected - %d VLANs" : "Connected (no VLANs parsed)", swcfg.vlan_n);
        swcfg.ok = swcfg.vlan_n > 0;
    }
    ssh_cleanup(sock, sess, ch);
    swcfg.busy = false;
    vTaskDelete(NULL);
}

static void apply_task(void *a)
{
    (void)a;
    swcfg.result[0] = 0; swcfg.ok = false;
    st("Applying VLAN %d to %s...", s_vlan, s_port);
    int sock = -1; LIBSSH2_CHANNEL *ch = NULL;
    LIBSSH2_SESSION *sess = ssh_login(&sock, &ch);
    if (ch) {
        char cmd[80];
        send_cmd(ch, "configure terminal", 1500, NULL, 0);
        snprintf(cmd, sizeof(cmd), "vlan %d", s_vlan);
        send_cmd(ch, cmd, 1500, NULL, 0);
        snprintf(cmd, sizeof(cmd), "untagged ethernet %s", s_port);
        send_cmd(ch, cmd, 2000, NULL, 0);
        send_cmd(ch, "end", 1200, NULL, 0);
        send_cmd(ch, "write memory", 3000, NULL, 0);
        // success heuristic: no "Error"/"Invalid" in the captured output
        swcfg.ok = !ci_str(swcfg.result, "error") && !ci_str(swcfg.result, "invalid") &&
                   !ci_str(swcfg.result, "incomplete");
        st(swcfg.ok ? "Applied - port %s now VLAN %d" : "Switch reported a problem - check output", s_port, s_vlan);
    }
    ssh_cleanup(sock, sess, ch);
    swcfg.busy = false;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------
static void build_preview(const char *port, int vlan)
{
    snprintf(swcfg.preview, sizeof(swcfg.preview),
             "conf t / vlan %d / untagged ethernet %s / end / write memory", vlan, port);
}

void switch_cfg_connect(const char *host, const char *user, const char *pass, const char *port)
{
    if (swcfg.busy) return;
    snprintf(s_host, sizeof(s_host), "%s", host ? host : "");
    snprintf(s_user, sizeof(s_user), "%s", user ? user : "");
    snprintf(s_pass, sizeof(s_pass), "%s", pass ? pass : "");
    snprintf(s_port, sizeof(s_port), "%s", port ? port : "");
    snprintf(swcfg.host, sizeof(swcfg.host), "%s", s_host);
    snprintf(swcfg.user, sizeof(swcfg.user), "%s", s_user);
    snprintf(swcfg.port, sizeof(swcfg.port), "%s", s_port);
    swcfg.busy = true;
    if (xTaskCreate(connect_task, "sw_conn", 16384, NULL, 4, NULL) != pdPASS) swcfg.busy = false;
}

void switch_cfg_apply(const char *host, const char *user, const char *pass, const char *port, int vlan)
{
    if (swcfg.busy) return;
    snprintf(s_host, sizeof(s_host), "%s", host ? host : "");
    snprintf(s_user, sizeof(s_user), "%s", user ? user : "");
    snprintf(s_pass, sizeof(s_pass), "%s", pass ? pass : "");
    snprintf(s_port, sizeof(s_port), "%s", port ? port : "");
    s_vlan = vlan;
    snprintf(swcfg.port, sizeof(swcfg.port), "%s", s_port);
    build_preview(s_port, vlan);
    swcfg.busy = true;
    if (xTaskCreate(apply_task, "sw_apply", 16384, NULL, 4, NULL) != pdPASS) swcfg.busy = false;
}

int switch_cfg_json(char *b, size_t cap)
{
    int o = snprintf(b, cap, "{\"busy\":%s,\"ok\":%s,\"connected\":%s,\"host\":\"%s\",\"port\":\"%s\",\"cur\":%d,\"status\":\"",
                     swcfg.busy ? "true" : "false", swcfg.ok ? "true" : "false", swcfg.connected ? "true" : "false",
                     swcfg.host, swcfg.port, swcfg.cur_vlan);
    char e[200];
    json_esc(e, sizeof(e), swcfg.status); o += snprintf(b + o, cap - o, "%s\",\"vlans\":[", e);
    for (int i = 0; i < swcfg.vlan_n && o < (int)cap - 40; i++)
        o += snprintf(b + o, cap - o, "%s%d", i ? "," : "", swcfg.vlans[i]);
    json_esc(e, sizeof(e), swcfg.preview); o += snprintf(b + o, cap - o, "],\"preview\":\"%s\",\"result\":\"", e);
    json_esc(e, sizeof(e), swcfg.result); o += snprintf(b + o, cap - o, "%s\"}", e);
    return o < (int)cap ? o : (int)cap - 1;
}
