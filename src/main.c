// =====================================================================
//  NetAlly Buster  -  handheld network tester on the Waveshare ESP32-S3-ETH
//  Built on the SENTRY node firmware: keeps the environment gauges, history,
//  trend warnings, weather, MQTT and switch discovery, and adds AutoTest,
//  port statistics, Wi-Fi scanner, technician tools, a phone GUI (hotspot +
//  captive portal) and a Bluetooth LE service for the companion app.
// =====================================================================
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mqtt_client.h"
#include "driver/temperature_sensor.h"
#include "lvgl.h"
#include "buster.h"
#include "display.h"
#include "ui.h"
#include "json.h"
#include "secrets.h"

static const char *TAG = "buster";

bst_t g;

// module entry points
void eth_start(void);
void wifi_start(void);
void ble_start(void);
void web_start(void);
void battery_start(void);

// ---------------------------------------------------------------------
// shared helpers
// ---------------------------------------------------------------------
static SemaphoreHandle_t s_lock;
void bst_lock(void)   { if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
void bst_unlock(void) { if (s_lock) xSemaphoreGiveRecursive(s_lock); }

static volatile uint32_t s_events;
void ui_post_event(uint32_t ev) { __atomic_fetch_or(&s_events, ev, __ATOMIC_SEQ_CST); }
uint32_t ui_take_events(void)   { return __atomic_exchange_n(&s_events, 0, __ATOMIC_SEQ_CST); }

int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
float c2f(float c) { return c * 9.0f / 5.0f + 32.0f; }
const char *rssi_quality(int r)
{
    return r >= -55 ? "Excellent" : r >= -67 ? "Good" : r >= -75 ? "Fair" : r >= -85 ? "Weak" : "Very weak";
}

// ---------------------------------------------------------------------
// event log (shown in the web GUI and the report)
// ---------------------------------------------------------------------
#define EVLOG_N 40
typedef struct { int64_t ms; time_t epoch; char txt[60]; } ev_t;
static ev_t s_ev[EVLOG_N];
static int s_ev_head, s_ev_cnt;

void evlog(const char *fmt, ...)
{
    char b[60];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    bst_lock();
    ev_t *e = &s_ev[s_ev_head];
    e->ms = now_ms(); e->epoch = time(NULL);
    snprintf(e->txt, sizeof(e->txt), "%s", b);
    s_ev_head = (s_ev_head + 1) % EVLOG_N;
    if (s_ev_cnt < EVLOG_N) s_ev_cnt++;
    bst_unlock();
    ESP_LOGI("event", "%s", b);
}

int evlog_json(char *out, size_t cap)
{
    size_t o = snprintf(out, cap, "[");
    bst_lock();
    for (int i = 0; i < s_ev_cnt && o < cap; i++) {
        ev_t *e = &s_ev[(s_ev_head - 1 - i + EVLOG_N) % EVLOG_N];   // newest first
        char tb[12] = "";
        if (e->epoch > 1600000000) { struct tm tm; localtime_r(&e->epoch, &tm); strftime(tb, sizeof(tb), "%H:%M:%S", &tm); }
        char x[80]; json_esc(x, sizeof(x), e->txt);
        o += snprintf(out + o, cap - o, "%s{\"t\":\"%s\",\"up\":%ld,\"m\":\"%s\"}", i ? "," : "", tb, (long)(e->ms / 1000), x);
    }
    bst_unlock();
    if (o < cap) o += snprintf(out + o, cap - o, "]");
    return o < cap ? (int)o : (int)cap - 1;
}

static void reboot_task(void *a) { (void)a; vTaskDelay(pdMS_TO_TICKS(800)); esp_restart(); }
void device_reboot(void) { xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL); }


// ---------------------------------------------------------------------
// screen task: owns LVGL
// ---------------------------------------------------------------------
static void ui_task(void *a)
{
    (void)a;
    display_init();
    display_set_brightness(cfg.bright);
    g.screen_on = true;
    ui_init();
    int64_t last = 0;
    while (1) {
        uint32_t wait = lv_timer_handler();
        int64_t t = now_ms();
        if (t - last >= 300) {
            last = t;
            ui_update();
            // screen sleep: dark after cfg.sleep_s idle, any touch wakes it
            uint32_t idle = lv_disp_get_inactive_time(NULL);
            if (cfg.sleep_s && g.screen_on && idle > cfg.sleep_s * 1000U) {
                g.screen_on = false;
                display_set_brightness(0);
            } else if (!g.screen_on && idle < 1000) {
                g.screen_on = true;
                display_set_brightness(cfg.bright);
            }
        }
        if (wait > 10) wait = 10;
        if (wait < 2) wait = 2;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

// ---------------------------------------------------------------------
// device health
// ---------------------------------------------------------------------
static void device_task(void *a)
{
    (void)a;
    temperature_sensor_handle_t ts = NULL;
    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&tc, &ts) == ESP_OK) temperature_sensor_enable(ts);
    while (1) {
        float c;
        if (ts && temperature_sensor_get_celsius(ts, &c) == ESP_OK) g.chip_temp = c;
        g.heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        g.heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        static int n;
        if (++n % 15 == 0) ESP_LOGI(TAG, "heap %u free, %u lowest", (unsigned)g.heap_free, (unsigned)g.heap_min);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static const char *reset_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB:      return "usb";
    case ESP_RST_EXT:      return "reset pin";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default:               return "other";
    }
}

// ---------------------------------------------------------------------
// MQTT to the SENTRY hub (same topics as before)
// ---------------------------------------------------------------------
static esp_mqtt_client_handle_t s_mqtt;
static char s_topic_data[48], s_topic_status[48];

static void mqtt_event(void *args, esp_event_base_t base, int32_t id, void *data)
{
    (void)args; (void)base; (void)data;
    if (id == MQTT_EVENT_CONNECTED) {
        if (!g.mqtt_conn) evlog("MQTT hub connected");
        g.mqtt_conn = true;
        esp_mqtt_client_publish(s_mqtt, s_topic_status, "online", 0, 1, 1);
    } else if (id == MQTT_EVENT_DISCONNECTED) g.mqtt_conn = false;
}

static void mqtt_task(void *a)
{
    (void)a;
    static char json[1024];
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (g.mqtt_conn) {
            json_mqtt(json, sizeof(json));
            esp_mqtt_client_publish(s_mqtt, s_topic_data, json, 0, 0, 0);
        }
    }
}

static void mqtt_start(void)
{
    if (!cfg.mqtt[0]) { ESP_LOGI(TAG, "MQTT broker not set - skipping"); return; }
    snprintf(s_topic_data, sizeof(s_topic_data), "sentry/%s/data", g.node_id);
    snprintf(s_topic_status, sizeof(s_topic_status), "sentry/%s/status", g.node_id);
    esp_mqtt_client_config_t mc = {
        .broker.address.uri = cfg.mqtt,
        .session.keepalive = 15,
        .session.last_will = { .topic = s_topic_status, .msg = "offline", .msg_len = 7, .qos = 1, .retain = 1 },
        .network.reconnect_timeout_ms = 5000,
    };
    s_mqtt = esp_mqtt_client_init(&mc);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_mqtt_client_start(s_mqtt);
    xTaskCreate(mqtt_task, "mqtt_pub", 4096, NULL, 2, NULL);
}

// ---------------------------------------------------------------------
// weather (OpenWeatherMap, Orlando) - kept from SENTRY
// ---------------------------------------------------------------------
#define OWM_LAT "28.54"
#define OWM_LON "-81.38"
static char wx_buf[1536];
static int wx_len;

static esp_err_t wx_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && wx_len + e->data_len < (int)sizeof(wx_buf) - 1) {
        memcpy(wx_buf + wx_len, e->data, e->data_len);
        wx_len += e->data_len;
        wx_buf[wx_len] = 0;
    }
    return ESP_OK;
}

static void weather_task(void *a)
{
    (void)a;
    vTaskDelay(pdMS_TO_TICKS(10000));
    while (1) {
        if (g.eth_ip || g.wifi_conn) {
            char url[200];
            snprintf(url, sizeof(url), "http://api.openweathermap.org/data/2.5/weather?lat=%s&lon=%s&units=imperial&appid=%s",
                     OWM_LAT, OWM_LON, cfg.owm);
            wx_len = 0; wx_buf[0] = 0;
            esp_http_client_config_t hc = { .url = url, .event_handler = wx_evt, .timeout_ms = 8000 };
            esp_http_client_handle_t c = esp_http_client_init(&hc);
            esp_err_t err = esp_http_client_perform(c);
            int st = esp_http_client_get_status_code(c);
            esp_http_client_cleanup(c);
            if (err == ESP_OK && st == 200) {
                const char *m = strstr(wx_buf, "\"temp\":");
                if (m) g.wx_temp = (int)strtof(m + 7, NULL);
                const char *w = strstr(wx_buf, "\"weather\":[");
                const char *mm = w ? strstr(w, "\"main\":\"") : NULL;
                if (mm) {
                    mm += 8;
                    int i = 0;
                    while (mm[i] && mm[i] != '"' && i < (int)sizeof(g.wx_cond) - 1) { g.wx_cond[i] = mm[i]; i++; }
                    g.wx_cond[i] = 0;
                }
                g.wx_valid = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(g.wx_valid ? 600000 : 60000));
    }
}

// ---------------------------------------------------------------------
static void services_task(void *a)
{
    (void)a;
    vTaskDelay(pdMS_TO_TICKS(700));   // let the screen allocate first
#define HEAP(step) ESP_LOGI(TAG, "free heap after %-8s %u (largest block %u)", step, \
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL))
    HEAP("screen");
    battery_start();
    eth_start();      HEAP("ethernet");
    wifi_start();     HEAP("wifi");
    ble_start();      HEAP("bluetooth");
    web_start();      HEAP("web");

    setenv("TZ", cfg.tz, 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    mqtt_start();
    if (cfg.owm[0]) xTaskCreate(weather_task, "weather", 6144, NULL, 2, NULL);
    xTaskCreate(device_task, "device", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "all services up, free heap %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    vTaskDelete(NULL);
}

void app_main(void)
{
    g.boot_us = esp_timer_get_time();
    s_lock = xSemaphoreCreateRecursiveMutex();
    snprintf(g.reset_reason, sizeof(g.reset_reason), "%s", reset_str(esp_reset_reason()));
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    ESP_LOGI(TAG, "%s v%s", FW_NAME, FW_VERSION);

    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    // SENTRY kept a 23 KB sensor history blob in NVS; free that space
    nvs_handle_t h;
    if (nvs_open("hist", NVS_READWRITE, &h) == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }

    cfg_load();
    evlog("Boot (%s)", g.reset_reason);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // screen first so the splash plays while everything else starts
    xTaskCreatePinnedToCore(ui_task, "ui", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(services_task, "svc", 6144, NULL, 4, NULL, 0);
}
