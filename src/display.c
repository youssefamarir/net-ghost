// NetAlly Buster - GC9A01 round LCD (DMA, async flush) + CST816S touch + LVGL port
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "lvgl.h"
#include "board.h"
#include "display.h"

static const char *TAG = "disp";

// ---------------- panel ----------------
typedef struct { uint8_t cmd; uint8_t data[12]; uint8_t len; uint16_t delay_ms; } lcd_init_t;

// exact register sequence from the SENTRY firmware (known good on this panel)
static const lcd_init_t gc9a01_init[] = {
    {0xEF, {0}, 0, 0},
    {0xEB, {0x14}, 1, 0},
    {0xFE, {0}, 0, 0},
    {0xEF, {0}, 0, 0},
    {0xEB, {0x14}, 1, 0},
    {0x84, {0x40}, 1, 0},
    {0x85, {0xFF}, 1, 0},
    {0x86, {0xFF}, 1, 0},
    {0x87, {0xFF}, 1, 0},
    {0x88, {0x0A}, 1, 0},
    {0x89, {0x21}, 1, 0},
    {0x8A, {0x00}, 1, 0},
    {0x8B, {0x80}, 1, 0},
    {0x8C, {0x01}, 1, 0},
    {0x8D, {0x01}, 1, 0},
    {0x8E, {0xFF}, 1, 0},
    {0x8F, {0xFF}, 1, 0},
    {0xB6, {0x00, 0x20}, 2, 0},
    {0x36, {0x08}, 1, 0},
    {0x3A, {0x05}, 1, 0},
    {0x90, {0x08, 0x08, 0x08, 0x08}, 4, 0},
    {0xBD, {0x06}, 1, 0},
    {0xBC, {0x00}, 1, 0},
    {0xFF, {0x60, 0x01, 0x04}, 3, 0},
    {0xC3, {0x13}, 1, 0},
    {0xC4, {0x13}, 1, 0},
    {0xC9, {0x22}, 1, 0},
    {0xBE, {0x11}, 1, 0},
    {0xE1, {0x10, 0x0E}, 2, 0},
    {0xDF, {0x21, 0x0C, 0x02}, 3, 0},
    {0xF0, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6, 0},
    {0xF1, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6, 0},
    {0xF2, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6, 0},
    {0xF3, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6, 0},
    {0xED, {0x1B, 0x0B}, 2, 0},
    {0xAE, {0x77}, 1, 0},
    {0xCD, {0x63}, 1, 0},
    {0x70, {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03}, 9, 0},
    {0xE8, {0x34}, 1, 0},
    {0x62, {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70}, 12, 0},
    {0x63, {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70}, 12, 0},
    {0x64, {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}, 7, 0},
    {0x66, {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}, 10, 0},
    {0x67, {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}, 10, 0},
    {0x74, {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00}, 7, 0},
    {0x98, {0x3E, 0x07}, 2, 0},
    {0x35, {0}, 0, 0},
    {0x21, {0}, 0, 0},
    {0x11, {0}, 0, 120},
    {0x29, {0}, 0, 20},
};

static esp_lcd_panel_io_handle_t s_io;
static lv_disp_drv_t s_drv;
static lv_disp_draw_buf_t s_buf;
#define BUF_LINES 24

static bool color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    (void)io; (void)e;
    lv_disp_drv_t *d = (lv_disp_drv_t *)ctx;
    if (d->draw_buf) lv_disp_flush_ready(d);      // ignore the boot-time clear transfers
    return false;
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px)
{
    (void)drv;
    uint8_t ca[4] = { a->x1 >> 8, a->x1 & 0xFF, a->x2 >> 8, a->x2 & 0xFF };
    uint8_t ra[4] = { a->y1 >> 8, a->y1 & 0xFF, a->y2 >> 8, a->y2 & 0xFF };
    esp_lcd_panel_io_tx_param(s_io, 0x2A, ca, 4);
    esp_lcd_panel_io_tx_param(s_io, 0x2B, ra, 4);
    size_t n = (size_t)(a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1) * 2;
    esp_lcd_panel_io_tx_color(s_io, 0x2C, px, n);   // finishes in color_done()
}

// ---------------- touch ----------------
static i2c_master_bus_handle_t s_tp_bus;
static i2c_master_dev_handle_t s_tp;

static bool tp_read(uint16_t *x, uint16_t *y)
{
    uint8_t reg = 0x00, d[7] = {0};
    if (i2c_master_transmit_receive(s_tp, &reg, 1, d, 7, 20) != ESP_OK) return false;
    if ((d[2] & 0x0F) == 0) return false;
    uint16_t rx = ((d[3] & 0x0F) << 8) | d[4];
    uint16_t ry = ((d[5] & 0x0F) << 8) | d[6];
    if (rx >= LCD_W || ry >= LCD_H) return false;
    *x = rx; *y = ry;
    return true;
}

static void touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    static uint16_t lx, ly;
    uint16_t x, y;
    if (tp_read(&x, &y)) { lx = x; ly = y; data->state = LV_INDEV_STATE_PRESSED; }
    else data->state = LV_INDEV_STATE_RELEASED;
    data->point.x = lx; data->point.y = ly;
}

static void tick_cb(void *a) { (void)a; lv_tick_inc(2); }

// backlight PWM (0 = off)
static bool s_pwm;
void display_set_brightness(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (!s_pwm) { gpio_set_level(LCD_BL, pct > 0); return; }
    uint32_t duty = (uint32_t)pct * 255 / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void display_init(void)
{
    // backlight + reset
    gpio_config_t io = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = (1ULL << LCD_RST) | (1ULL << LCD_BL) };
    gpio_config(&io);
    gpio_set_level(LCD_BL, 0);
    gpio_set_level(LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t bus = { .sclk_io_num = LCD_CLK, .mosi_io_num = LCD_MOSI, .miso_io_num = -1,
                             .quadwp_io_num = -1, .quadhd_io_num = -1,
                             .max_transfer_sz = LCD_W * BUF_LINES * 2 + 16 };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

    lv_init();

    esp_lcd_panel_io_spi_config_t ioc = {
        .cs_gpio_num = LCD_CS, .dc_gpio_num = LCD_DC, .spi_mode = 0, .pclk_hz = LCD_SPI_HZ,
        .trans_queue_depth = 4, .on_color_trans_done = color_done, .user_ctx = &s_drv,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &ioc, &s_io));
    for (size_t i = 0; i < sizeof(gc9a01_init) / sizeof(gc9a01_init[0]); i++) {
        const lcd_init_t *c = &gc9a01_init[i];
        esp_lcd_panel_io_tx_param(s_io, c->cmd, c->len ? c->data : NULL, c->len);
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }

    // clear to black before the backlight comes on
    size_t bsz = LCD_W * BUF_LINES * sizeof(lv_color_t);
    lv_color_t *b1 = heap_caps_malloc(bsz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lv_color_t *b2 = heap_caps_malloc(bsz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(b1 && b2);
    memset(b1, 0, bsz);
    for (int y = 0; y < LCD_H; y += BUF_LINES) {
        uint8_t ca[4] = { 0, 0, 0, LCD_W - 1 };
        uint8_t ra[4] = { 0, y, 0, y + BUF_LINES - 1 };
        esp_lcd_panel_io_tx_param(s_io, 0x2A, ca, 4);
        esp_lcd_panel_io_tx_param(s_io, 0x2B, ra, 4);
        esp_lcd_panel_io_tx_color(s_io, 0x2C, b1, bsz);
    }
    vTaskDelay(pdMS_TO_TICKS(30));

    lv_disp_draw_buf_init(&s_buf, b1, b2, LCD_W * BUF_LINES);
    lv_disp_drv_init(&s_drv);
    s_drv.hor_res = LCD_W;
    s_drv.ver_res = LCD_H;
    s_drv.flush_cb = flush_cb;
    s_drv.draw_buf = &s_buf;
    lv_disp_drv_register(&s_drv);

    // touch
    gpio_config_t tr = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << TP_RST };
    gpio_config(&tr);
    gpio_set_level(TP_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TP_RST, 1); vTaskDelay(pdMS_TO_TICKS(100));
    i2c_master_bus_config_t tb = { .clk_source = I2C_CLK_SRC_DEFAULT, .i2c_port = I2C_NUM_0,
                                   .scl_io_num = TP_SCL, .sda_io_num = TP_SDA, .glitch_ignore_cnt = 7,
                                   .flags.enable_internal_pullup = true };
    ESP_ERROR_CHECK(i2c_new_master_bus(&tb, &s_tp_bus));
    i2c_device_config_t td = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TP_ADDR, .scl_speed_hz = 400000 };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_tp_bus, &td, &s_tp));
    static lv_indev_drv_t id;
    lv_indev_drv_init(&id);
    id.type = LV_INDEV_TYPE_POINTER;
    id.read_cb = touch_cb;
    lv_indev_drv_register(&id);

    const esp_timer_create_args_t ta = { .callback = tick_cb, .name = "lv_tick" };
    esp_timer_handle_t th;
    esp_timer_create(&ta, &th);
    esp_timer_start_periodic(th, 2000);

    ledc_timer_config_t lt = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
                               .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_channel_config_t lc = { .gpio_num = LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
                                 .timer_sel = LEDC_TIMER_0, .duty = 204, .hpoint = 0 };
    s_pwm = ledc_timer_config(&lt) == ESP_OK && ledc_channel_config(&lc) == ESP_OK;
    if (!s_pwm) gpio_set_level(LCD_BL, 1);
    ESP_LOGI(TAG, "GC9A01 up (%d MHz SPI, DMA)", LCD_SPI_HZ / 1000000);
}
