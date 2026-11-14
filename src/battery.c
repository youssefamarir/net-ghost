
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "buster.h"
#include "board.h"

static const char *TAG = "batt";
static adc_oneshot_unit_handle_t s_adc;


adc_oneshot_unit_handle_t adc1_handle(void)
{
    static adc_oneshot_unit_handle_t h;
    static bool tried;
    if (!tried) {
        tried = true;
        adc_oneshot_unit_init_cfg_t uc = { .unit_id = ADC_UNIT_1 };
        if (adc_oneshot_new_unit(&uc, &h) != ESP_OK) h = NULL;
    }
    return h;
}
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;


static int volts_to_pct(float v)
{
    static const float tv[] = { 4.20f, 4.10f, 4.00f, 3.90f, 3.80f, 3.70f, 3.60f, 3.50f, 3.40f, 3.30f };
    static const int   tp[] = { 100,   90,    80,    68,    55,    40,    25,    12,    5,     0 };
    if (v >= tv[0]) return 100;
    for (int i = 1; i < 10; i++)
        if (v >= tv[i]) return tp[i] + (int)((v - tv[i]) / (tv[i - 1] - tv[i]) * (tp[i - 1] - tp[i]));
    return 0;
}

static void batt_task(void *a)
{
    (void)a;
    float avg = 0;
    bool warned = false;
    while (1) {
        if (cfg.battery && s_adc) {
            int sum = 0, n = 0;
            for (int i = 0; i < 16; i++) {
                int raw, mv;
                if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK) continue;
                if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) sum += mv;
                else sum += raw * 3100 / 4095;
                n++;
            }
            float v = n ? (sum / (float)n) / 1000.0f * BATT_DIVIDER : 0;
            avg = (avg < 0.5f) ? v : avg * 0.8f + v * 0.2f;
            g.batt_v = avg;
            g.batt_present = avg > 2.8f && avg < 4.6f;
            g.batt_pct = g.batt_present ? volts_to_pct(avg) : 0;
#if BATT_CHG_GPIO >= 0
            g.batt_charging = gpio_get_level(BATT_CHG_GPIO) == 0;
#else
            g.batt_charging = false;
#endif
            if (g.batt_present && g.batt_pct <= 15 && !g.batt_charging && !warned) {
                warned = true;
                ui_post_event(EV_BATT_LOW);
                evlog("Battery low (%d%%)", g.batt_pct);
            }
            if (g.batt_pct > 25 || g.batt_charging) warned = false;
        } else {
            g.batt_present = false;
            g.batt_pct = 0;
            g.batt_v = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void battery_start(void)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(BATT_ADC_GPIO, &unit, &s_chan) == ESP_OK && unit == ADC_UNIT_1) {
        s_adc = adc1_handle();
        if (s_adc) {
            adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
            adc_oneshot_config_channel(s_adc, s_chan, &cc);
            adc_cali_curve_fitting_config_t cf = { .unit_id = ADC_UNIT_1, .chan = s_chan,
                                                   .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
            if (adc_cali_create_scheme_curve_fitting(&cf, &s_cali) != ESP_OK) s_cali = NULL;
        }
    }
#if BATT_CHG_GPIO >= 0
    gpio_config_t io = { .mode = GPIO_MODE_INPUT, .pin_bit_mask = 1ULL << BATT_CHG_GPIO,
                         .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);
#endif
    ESP_LOGI(TAG, "battery monitor on GPIO%d (%s)", BATT_ADC_GPIO, cfg.battery ? "enabled" : "off until enabled in Settings");
    xTaskCreate(batt_task, "batt", 3072, NULL, 2, NULL);
}
