

































#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_http_client.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "buster.h"
#include "json.h"

void ble_store_config_init(void);

static const char *TAG = "ble";

#define BUSTER_UUID(x) BLE_UUID128_INIT(0x21, 0x52, 0x45, 0x54, 0x53, 0x55, 0x42, 0x59, \
                                        0x4c, 0x4c, 0x41, 0x4e, (x) & 0xff, ((x) >> 8) & 0xff, 0xde, 0xc0)

static const ble_uuid128_t svc_uuid  = BUSTER_UUID(0x0000);
static const ble_uuid128_t u_sum     = BUSTER_UUID(0x0001);
static const ble_uuid128_t u_link    = BUSTER_UUID(0x0010);
static const ble_uuid128_t u_switch  = BUSTER_UUID(0x0011);
static const ble_uuid128_t u_wifi    = BUSTER_UUID(0x0012);
static const ble_uuid128_t u_test    = BUSTER_UUID(0x0014);
static const ble_uuid128_t u_dev     = BUSTER_UUID(0x0015);
static const ble_uuid128_t u_alerts  = BUSTER_UUID(0x0016);
static const ble_uuid128_t u_qual    = BUSTER_UUID(0x0017);
static const ble_uuid128_t u_cfg     = BUSTER_UUID(0x0018);
static const ble_uuid128_t u_gui     = BUSTER_UUID(0x0020);
static const ble_uuid128_t u_cmd     = BUSTER_UUID(0x0030);
static const ble_uuid128_t u_apireq  = BUSTER_UUID(0x0040);
static const ble_uuid128_t u_apirsp  = BUSTER_UUID(0x0041);

enum { C_SUM, C_LINK, C_SWITCH, C_WIFI, C_TEST, C_DEV, C_GUI, C_CMD, C_ALERTS, C_QUAL, C_CFG, C_APIREQ, C_APIRSP };

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_sum_handle, s_rsp_handle;
static bool s_sum_sub, s_rsp_sub;
static uint8_t s_addr_type;
static bool s_synced;
static char s_buf[4096];




static void bonds_count(void)
{
    int n = 0;
    if (ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &n) == 0) g.ble_bonds = n;
}

static bool link_ok(uint16_t conn)
{
    if (!cfg.ble_pin) return true;
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) != 0) return false;
    return d.sec_state.encrypted && d.sec_state.authenticated;
}




static void handle_cmd(char *c)
{
    ESP_LOGI(TAG, "cmd: %s", c);
    if (!strcmp(c, "autotest")) autotest_start();
    else if (!strcmp(c, "scan")) wifi_scan_start();
    else if (!strcmp(c, "blink")) eth_blink_start(cfg.blink_s);
    else if (!strcmp(c, "reboot")) device_reboot();
    else if (!strcmp(c, "sweep")) tool_start("sweep", "");
    else if (!strcmp(c, "quality")) quality_start();
    else if (!strcmp(c, "renew")) eth_dhcp_renew();
    else if (!strcmp(c, "speed")) tool_start("speed", "");
    else if (!strcmp(c, "wspeed")) tool_start("wspeed", "");
    else if (!strcmp(c, "espeed")) tool_start("espeed", "");
    else if (!strcmp(c, "speedstop")) speed_cancel();
    else if (!strncmp(c, "set=", 4)) {
        char *k = c + 4, *eq = strchr(k, '=');
        if (eq) { *eq = 0; cfg_apply(cfg_set(k, eq + 1)); }
    }
    else if (!strncmp(c, "wifi=", 5)) {
        char *ssid = c + 5, *nl = strchr(ssid, '\n');
        char *pass = "";
        if (nl) { *nl = 0; pass = nl + 1; }
        if (ssid[0]) wifi_set_credentials(ssid, pass);
    } else {
        char *eq = strchr(c, '=');
        if (eq) { *eq = 0; tool_start(c, eq + 1); }
    }
}




#define API_MAX 2048
static char s_req[API_MAX + 1];
static int  s_req_len;
static char s_job[API_MAX + 1];
static int  s_job_len;
static uint8_t s_job_id, s_req_id;
static volatile bool s_job_new, s_abort;
static SemaphoreHandle_t s_job_mx;
static TaskHandle_t s_api_task;

static void api_write(const uint8_t *d, int n)
{
    if (n < 2) return;
    uint8_t flags = d[0], id = d[1];
    d += 2; n -= 2;
    if (id != s_req_id) { s_req_id = id; s_req_len = 0; }
    if (s_req_len + n > API_MAX) n = API_MAX - s_req_len;
    memcpy(s_req + s_req_len, d, n);
    s_req_len += n;
    if (!(flags & 1)) return;
    xSemaphoreTake(s_job_mx, portMAX_DELAY);
    memcpy(s_job, s_req, s_req_len);
    s_job[s_req_len] = 0;
    s_job_len = s_req_len;
    s_job_id = id;
    s_job_new = true;
    s_abort = true;
    xSemaphoreGive(s_job_mx);
    s_req_len = 0;
    xTaskNotifyGive(s_api_task);
}


static bool rsp_send(char type, uint8_t id, const void *data, int n)
{
    for (int tries = 0; tries < 200; tries++) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_rsp_sub) return false;
        uint8_t hdr[2] = { (uint8_t)type, id };
        struct os_mbuf *om = ble_hs_mbuf_from_flat(hdr, 2);
        if (om && n > 0 && os_mbuf_append(om, data, n) != 0) { os_mbuf_free_chain(om); om = NULL; }
        if (om) {
            int rc = ble_gatts_notify_custom(s_conn, s_rsp_handle, om);
            if (rc == 0) return true;
            if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static char s_ctype[48];
static esp_err_t api_http_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "Content-Type"))
        snprintf(s_ctype, sizeof(s_ctype), "%s", e->header_value);
    return ESP_OK;
}

static void api_serve(uint8_t id, char *req)
{

    char *body = strchr(req, '\n');
    if (body) *body++ = 0; else body = "";
    char *sp = strchr(req, ' ');
    if (!sp || strncmp(sp + 1, "/api/", 5)) {
        rsp_send('H', id, "400\ntext/plain", 14); rsp_send('E', id, NULL, 0); return;
    }
    *sp = 0;
    const char *path = sp + 1;
    bool post = !strcmp(req, "POST");
    char url[300];
    snprintf(url, sizeof(url), "http://127.0.0.1%s", path);
    s_ctype[0] = 0;
    esp_http_client_config_t c = {
        .url = url, .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 10000, .buffer_size = 1024, .buffer_size_tx = 512,
        .event_handler = api_http_event, .disable_auto_redirect = true,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    int blen = strlen(body), status = 502;
    if (h && esp_http_client_open(h, post ? blen : 0) == ESP_OK) {
        if (post && blen) esp_http_client_write(h, body, blen);
        if (esp_http_client_fetch_headers(h) >= 0) status = esp_http_client_get_status_code(h);
    }
    char hd[64];
    int hn = snprintf(hd, sizeof(hd), "%d\n%s", status, s_ctype[0] ? s_ctype : "text/plain");
    bool ok = rsp_send('H', id, hd, hn);
    if (ok && status != 502) {
        int chunk = (g.ble_mtu > 23 ? g.ble_mtu : 23) - 3 - 2;
        if (chunk > 500) chunk = 500;
        static char b[512];
        while (!s_abort) {
            int n = esp_http_client_read(h, b, chunk);
            if (n <= 0) break;
            if (!rsp_send('D', id, b, n)) { ok = false; break; }
        }
    }
    if (ok) rsp_send('E', id, NULL, 0);
    if (h) esp_http_client_cleanup(h);
    g.ble_api_reqs++;
}

static void api_task(void *p)
{
    (void)p;
    static char job[API_MAX + 1];
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (s_job_new) {
            xSemaphoreTake(s_job_mx, portMAX_DELAY);
            memcpy(job, s_job, s_job_len + 1);
            uint8_t id = s_job_id;
            s_job_new = false;
            s_abort = false;
            xSemaphoreGive(s_job_mx);
            api_serve(id, job);
        }
    }
}




static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    int which = (int)(intptr_t)arg;
    if (!link_ok(conn)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        int n = 0;
        switch (which) {
        case C_SUM:    n = json_summary(s_buf, sizeof(s_buf)); break;
        case C_LINK:   n = json_link(s_buf, sizeof(s_buf)); break;
        case C_SWITCH: n = json_switch(s_buf, sizeof(s_buf)); break;
        case C_WIFI:   n = json_wifi(s_buf, sizeof(s_buf), 4); break;
        case C_TEST:   n = json_test(s_buf, sizeof(s_buf), false); break;
        case C_DEV:    n = json_device(s_buf, sizeof(s_buf)); break;
        case C_GUI:    n = json_gui(s_buf, sizeof(s_buf)); break;
        case C_ALERTS: n = json_alerts(s_buf, sizeof(s_buf)); break;
        case C_QUAL:   n = json_quality(s_buf, sizeof(s_buf)); break;
        case C_CFG:    n = cfg_json(s_buf, sizeof(s_buf)); break;
        default: break;
        }
        if (n > 512) n = 512;
        if (n < 0) n = 0;
        return os_mbuf_append(ctxt->om, s_buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && (which == C_CMD || which == C_APIREQ)) {
        static uint8_t in[520]; uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, in, sizeof(in) - 1, &len) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (which == C_APIREQ) { api_write(in, len); return 0; }
        in[len] = 0;
        handle_cmd((char *)in);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

#define RD(U, id) { .uuid = &U.u, .access_cb = chr_access, .arg = (void *)(id), .flags = BLE_GATT_CHR_F_READ }

static const struct ble_gatt_svc_def svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &u_sum.u, .access_cb = chr_access, .arg = (void *)C_SUM,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_sum_handle },
            RD(u_link, C_LINK),
            RD(u_switch, C_SWITCH),
            RD(u_wifi, C_WIFI),
            RD(u_test, C_TEST),
            RD(u_dev, C_DEV),
            RD(u_gui, C_GUI),
            RD(u_alerts, C_ALERTS),
            RD(u_qual, C_QUAL),
            RD(u_cfg, C_CFG),
            { .uuid = &u_cmd.u, .access_cb = chr_access, .arg = (void *)C_CMD,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &u_apireq.u, .access_cb = chr_access, .arg = (void *)C_APIREQ,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &u_apirsp.u, .access_cb = chr_access, .arg = (void *)C_APIRSP,
              .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_rsp_handle },
            { 0 },
        },
    },
    { 0 },
};




static void adv_start(void);

static void sec_update(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    g.ble_secure = ble_gap_conn_find(conn, &d) == 0 && d.sec_state.encrypted && d.sec_state.authenticated;
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            g.ble_conn = true;
            g.ble_secure = false;
            g.ble_mtu = 23;
            ui_post_event(EV_BLE_CONN);
            ESP_LOGI(TAG, "phone connected");

            struct ble_gap_upd_params up = { .itvl_min = 12, .itvl_max = 24, .latency = 0,
                                             .supervision_timeout = 400 };
            ble_gap_update_params(s_conn, &up);
            ble_gap_set_data_len(s_conn, 251, 2120);
            ble_gap_set_prefered_le_phy(s_conn, BLE_GAP_LE_PHY_2M_MASK | BLE_GAP_LE_PHY_1M_MASK,
                                        BLE_GAP_LE_PHY_2M_MASK | BLE_GAP_LE_PHY_1M_MASK, 0);
            if (cfg.ble_pin) ble_gap_security_initiate(s_conn);
        } else adv_start();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_sum_sub = s_rsp_sub = false;
        g.ble_conn = g.ble_secure = g.ble_pairing = false;
        s_abort = true;
        ui_post_event(EV_BLE_DISC);
        adv_start();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_sum_handle) s_sum_sub = ev->subscribe.cur_notify;
        if (ev->subscribe.attr_handle == s_rsp_handle) s_rsp_sub = ev->subscribe.cur_notify;
        break;
    case BLE_GAP_EVENT_MTU:
        g.ble_mtu = ev->mtu.value;
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (ev->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP, .passkey = esp_random() % 1000000 };
            g.ble_passkey = io.passkey;
            g.ble_pairing = true;
            ble_sm_inject_io(ev->passkey.conn_handle, &io);
            ESP_LOGI(TAG, "pairing PIN %06u", (unsigned)io.passkey);
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        g.ble_pairing = false;
        sec_update(ev->enc_change.conn_handle);
        bonds_count();
        if (ev->enc_change.status == 0) { ui_post_event(EV_BLE_PAIRED); evlog("Bluetooth: phone paired"); }
        else { ui_post_event(EV_BLE_PAIRFAIL); evlog("Bluetooth: pairing failed (%d)", ev->enc_change.status); }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {

        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        break;
    default: break;
    }
    return 0;
}

static void adv_start(void)
{
    if (!s_synced || !cfg.ble || s_conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) return;
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&f) != 0) { ESP_LOGW(TAG, "adv fields"); return; }

    struct ble_hs_adv_fields r = {0};
    r.name = (uint8_t *)g.ble_name;
    r.name_len = strlen(g.ble_name);
    r.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&r);

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    p.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;
    if (ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL) == 0) g.ble_on = true;
}

void ble_apply(void)
{
    if (!s_synced) return;
    if (cfg.ble) { adv_start(); return; }
    ble_gap_adv_stop();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    g.ble_on = false;
    evlog("Bluetooth off");
}

void ble_forget_all(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    ble_store_clear();
    bonds_count();
    evlog("Bluetooth: paired phones forgotten");
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_addr_type);
    s_synced = true;
    bonds_count();
    adv_start();
    ESP_LOGI(TAG, "%s as \"%s\"", cfg.ble ? "advertising" : "ready (off)", g.ble_name);
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset %d", reason); g.ble_on = false; s_synced = false; }

static void host_task(void *p)
{
    (void)p;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void notify_task(void *p)
{
    (void)p;
    static char b[256];
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_sum_sub || !link_ok(s_conn)) continue;
        int n = json_summary(b, sizeof(b));
        int mtu = ble_att_mtu(s_conn) - 3;
        if (n > mtu) n = mtu;
        if (n <= 0) continue;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(b, n);
        if (om) ble_gatts_notify_custom(s_conn, s_sum_handle, om);
    }
}

void ble_start(void)
{
    snprintf(g.ble_name, sizeof(g.ble_name), "Net Ghost %s", g.ap_ssid + strlen(g.ap_ssid) - 4);
    s_job_mx = xSemaphoreCreateMutex();
    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "BLE init failed"); return; }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(svcs) || ble_gatts_add_svcs(svcs)) { ESP_LOGE(TAG, "GATT setup failed"); return; }
    ble_svc_gap_device_name_set(g.ble_name);
    ble_store_config_init();
    xTaskCreate(api_task, "ble_api", 6144, NULL, 4, &s_api_task);
    nimble_port_freertos_init(host_task);
    xTaskCreate(notify_task, "ble_ntf", 3072, NULL, 3, NULL);
}
