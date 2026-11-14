

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "lvgl.h"
#include "buster.h"
#include "ui.h"


#define C_BG      0x05080C
#define C_RING    0x0F1720
#define C_DIM     0x8498B0
#define C_TEXT    0xF4FAFF
#define C_CYAN    0x22D3EE
#define C_GREEN   0x4ADE80
#define C_AMBER   0xFBBF24
#define C_RED     0xF87171
#define C_VIOLET  0xA78BFA
#define C_ORANGE  0xFB923C
#define C_BLUE    0x60A5FA
#define C_GOLD    0xE6C478
#define C_PINK    0xF472B6

#define SCR 240
#define R   120




enum {
    P_HOME, P_DEV, P_POWER, P_ALERT,
    P_SWITCH, P_SWDET, P_POE, P_CABLE, P_PORT, P_PKT, P_IP, P_WIRE,
    P_WIFI, P_WSIG, P_WINFO, P_WIP, P_SCAN,
    P_TEST, P_WSPEED, P_ESPEED, P_QUAL, P_PING, P_HOSTS, P_BUFFER,
    P_SECROUTER, P_SECWIFI, P_SECROGUE, P_SECDNS, P_SECSNMP,
    P_SECUPNP, P_SECTLS, P_SECDHCP, P_SECSEG, P_SECARP,
    P_SET, P_DISPLAY, P_BTSET,
    P_CONN, P_BTCONN,
    P_COUNT };
enum { COL_DEV, COL_SW, COL_WIFI, COL_TOOLS, COL_SEC, COL_SET, COL_CONN, COL_COUNT };
static const uint8_t col_first[COL_COUNT + 1] = { P_HOME, P_SWITCH, P_WIFI, P_TEST, P_SECROUTER, P_SET, P_CONN, P_COUNT };
#define MAX_ROWS 10
static int page_col(int p) { int c = 0; while (c < COL_COUNT - 1 && p >= col_first[c + 1]) c++; return c; }
static int page_row(int p) { return p - col_first[page_col(p)]; }
static int col_rows(int c) { return col_first[c + 1] - col_first[c]; }

static lv_obj_t *tv;
static lv_obj_t *tiles[P_COUNT];
static lv_obj_t *rings[P_COUNT];
static int cur_page = 0;
static int s_display_cfg = -1;




static lv_obj_t *mk_label(lv_obj_t *p, const lv_font_t *f, uint32_t col, const char *txt)
{
    if (cfg.text_size) {
        if (f == &lv_font_montserrat_12) f = &lv_font_montserrat_14;
        else if (f == &lv_font_montserrat_14) f = &lv_font_montserrat_16;
    }
    if (col == C_DIM && cfg.contrast) col = 0xB7CCE0;
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_label_set_text(l, txt);
    return l;
}

static void set_txt(lv_obj_t *l, const char *s)
{
    if (!l) return;
    const char *o = lv_label_get_text(l);
    if (!o || strcmp(o, s) != 0) lv_label_set_text(l, s);
}

static void set_fmt(lv_obj_t *l, const char *fmt, ...)
{
    char b[160];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    set_txt(l, b);
}

static void set_col(lv_obj_t *o, uint32_t c)
{
    if (c == C_DIM && cfg.contrast) c = 0xB7CCE0;
    lv_color_t n = lv_color_hex(c);
    if (lv_obj_get_style_text_color(o, 0).full != n.full)
        lv_obj_set_style_text_color(o, n, 0);
}


static int band_w(int y0, int y1, int pad)
{
    int d0 = abs(y0 - R), d1 = abs(y1 - R);
    int d = d0 > d1 ? d0 : d1;
    if (d >= R) return 0;
    int w = (int)(2.0f * sqrtf((float)(R * R - d * d))) - pad;
    return w < 40 ? 40 : w;
}


static lv_obj_t *kv_row_ex(lv_obj_t *p, int y, int yabs, const char *key, lv_obj_t **val)
{
    int w = band_w(yabs, yabs + 17, 30);
    if (w > 184) w = 184;
    lv_obj_t *row = lv_obj_create(p);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, w, 18);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *k = mk_label(row, &lv_font_montserrat_12, C_DIM, key);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *v = mk_label(row, &lv_font_montserrat_14, C_TEXT, "--");
    lv_label_set_long_mode(v, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(v, w - lv_txt_get_width(key, strlen(key), cfg.text_size ? &lv_font_montserrat_14 : &lv_font_montserrat_12, 0, 0) - 8);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    if (val) *val = v;
    return row;
}
static lv_obj_t *kv_row(lv_obj_t *p, int y, const char *key, lv_obj_t **val)
{
    return kv_row_ex(p, y, y, key, val);
}

static lv_obj_t *center_label(lv_obj_t *p, const lv_font_t *f, uint32_t col, int y, const char *txt)
{
    lv_obj_t *l = mk_label(p, f, col, txt);
    const lv_font_t *font = lv_obj_get_style_text_font(l, 0);
    lv_obj_set_width(l, band_w(y, y + font->line_height, 22));
    lv_label_set_long_mode(l, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

static lv_obj_t *pill_btn(lv_obj_t *p, int y, int w, const char *txt, uint32_t col,
                          lv_event_cb_t cb, lv_obj_t **lbl)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, 26);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(b, 13, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_10, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(b, -3, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(b, -2, LV_STATE_PRESSED);
    lv_obj_t *l = mk_label(b, &lv_font_montserrat_12, col, txt);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (lbl) *lbl = l;
    return b;
}

static void fmt_dur(char *b, size_t n, int64_t s)
{
    if (s < 0) s = 0;
    if (s < 60)          snprintf(b, n, "%ds", (int)s);
    else if (s < 3600)   snprintf(b, n, "%dm %02ds", (int)(s / 60), (int)(s % 60));
    else if (s < 86400)  snprintf(b, n, "%dh %02dm", (int)(s / 3600), (int)((s % 3600) / 60));
    else                 snprintf(b, n, "%dd %dh", (int)(s / 86400), (int)((s % 86400) / 3600));
}

static void fmt_count(char *b, size_t n, uint32_t v)
{
    if (v < 10000)          snprintf(b, n, "%u", (unsigned)v);
    else if (v < 1000000)   snprintf(b, n, "%.1fk", v / 1000.0);
    else                    snprintf(b, n, "%.2fM", v / 1000000.0);
}


static void abbr_port(char *out, size_t n, const char *in)
{
    static const char *map[][2] = {
        {"TwentyFiveGigE", "Twe"}, {"TenGigabitEthernet", "Te"}, {"GigabitEthernet", "Gi"},
        {"FastEthernet", "Fa"}, {"Ethernet", "Eth"}, {"ethernet", "eth"},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        size_t L = strlen(map[i][0]);
        if (strncmp(in, map[i][0], L) == 0) { snprintf(out, n, "%s%s", map[i][1], in + L); return; }
    }
    snprintf(out, n, "%s", in);
}




static void a_ty(void *o, int32_t v)  { lv_obj_set_style_translate_y((lv_obj_t *)o, v, 0); }
static void a_opa(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, v, 0); }
static void a_arcv(void *o, int32_t v){ lv_arc_set_value((lv_obj_t *)o, v); }
static void a_y(void *o, int32_t v)   { lv_obj_set_y((lv_obj_t *)o, v); }
static void a_rot(void *o, int32_t v) { lv_arc_set_rotation((lv_obj_t *)o, v); }
static void a_size(void *o, int32_t v){ lv_obj_set_size((lv_obj_t *)o, v, v); lv_obj_center((lv_obj_t *)o); }

static void anim(void *obj, lv_anim_exec_xcb_t cb, int32_t a, int32_t b, uint32_t ms,
                 uint32_t delay, lv_anim_path_cb_t path)
{
    if (!cfg.animations) { cb(obj, b); return; }
    lv_anim_t an; lv_anim_init(&an);
    lv_anim_set_var(&an, obj);
    lv_anim_set_exec_cb(&an, cb);
    lv_anim_set_values(&an, a, b);
    lv_anim_set_time(&an, ms);
    lv_anim_set_delay(&an, delay);
    lv_anim_set_path_cb(&an, path ? path : lv_anim_path_ease_out);
    lv_anim_start(&an);
}


static void arc_to(lv_obj_t *arc, int v)
{
    int cur = lv_arc_get_value(arc);
    if (cur == v) return;
    lv_anim_del(arc, a_arcv);
    anim(arc, a_arcv, cur, v, 500, 0, lv_anim_path_ease_out);
}


static void page_enter(int idx)
{
    lv_obj_t *t = tiles[idx];
    lv_obj_set_style_bg_color(t, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_bg_grad_color(t, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
    uint32_t n = lv_obj_get_child_cnt(t);
    int k = 0;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(t, i);
        if (c == rings[idx]) continue;
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_anim_del(c, a_ty);
        lv_anim_del(c, a_opa);
        anim(c, a_ty, 14, 0, 300, k * 30, lv_anim_path_ease_out);
        k++;
    }
    if (rings[idx]) {
        lv_anim_del(rings[idx], a_arcv);
        anim(rings[idx], a_arcv, 0, 360, 650, 0, lv_anim_path_ease_in_out);
    }
}





static lv_obj_t *page_new(int idx, const char *title, uint32_t accent)
{
    lv_obj_t *t = tiles[idx];
    lv_obj_set_style_bg_color(t, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_bg_grad_color(t, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);

    lv_obj_t *ring = lv_arc_create(t);
    lv_obj_set_size(ring, 234, 234);
    lv_obj_center(ring);
    lv_arc_set_rotation(ring, 270);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_range(ring, 0, 360);
    lv_arc_set_value(ring, 360);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(ring, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ring, lv_color_hex(C_RING), LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(ring, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ring, false, LV_PART_INDICATOR);
    rings[idx] = ring;

    if (title) {
        lv_obj_t *l = center_label(t, &lv_font_montserrat_14, accent, 20, title);

        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_letter_space(l, 1, 0);
        lv_obj_set_width(l, band_w(20, 36, 8));
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    }
    return t;
}


static lv_obj_t *cdots[COL_COUNT], *rdots[MAX_ROWS];
static int dot_cx[COL_COUNT + MAX_ROWS], dot_cy[COL_COUNT + MAX_ROWS];
static void a_dot(void *o, int32_t v)
{
    int i = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)o);
    lv_obj_set_size((lv_obj_t *)o, v, v);
    lv_obj_set_pos((lv_obj_t *)o, dot_cx[i] - v / 2, dot_cy[i] - v / 2);
}
static lv_obj_t *dot_new(int i, float deg, float rad)
{
    float a = deg * 3.14159265f / 180.0f;
    dot_cx[i] = 120 + (int)lroundf(rad * cosf(a));
    dot_cy[i] = 120 + (int)lroundf(rad * sinf(a));
    lv_obj_t *d = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(d);
    lv_obj_set_user_data(d, (void *)(intptr_t)i);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(0x2A3442), 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    a_dot(d, 4);
    return d;
}

static void dots_create(void)
{
    for (int c = 0; c < COL_COUNT; c++)
        cdots[c] = dot_new(c, 90.0f + (c - (COL_COUNT - 1) / 2.0f) * 7.0f, 109.0f);
    for (int r = 0; r < MAX_ROWS; r++) rdots[r] = dot_new(COL_COUNT + r, 0, 110.0f);
}

static void dot_to(lv_obj_t *d, bool on, uint32_t col)
{
    int target = on ? 8 : 4;
    if (lv_obj_get_width(d) != target || on) {
        lv_anim_del(d, a_dot);
        anim(d, a_dot, lv_obj_get_width(d), target, 220, 0, lv_anim_path_overshoot);
    }
    lv_obj_set_style_bg_color(d, lv_color_hex(on ? col : 0x2A3442), 0);
}

static void dots_set(int page)
{
    int c = page_col(page), r = page_row(page), n = col_rows(c);
    for (int i = 0; i < COL_COUNT; i++) dot_to(cdots[i], i == c, C_CYAN);
    for (int i = 0; i < MAX_ROWS; i++) {
        if (i >= n || n < 2) { lv_obj_add_flag(rdots[i], LV_OBJ_FLAG_HIDDEN); continue; }

        int k = COL_COUNT + i;
        float a = ((i - (n - 1) / 2.0f) * 7.0f) * 3.14159265f / 180.0f;
        dot_cx[k] = 120 + (int)lroundf(110.0f * cosf(a));
        dot_cy[k] = 120 + (int)lroundf(110.0f * sinf(a));
        lv_obj_clear_flag(rdots[i], LV_OBJ_FLAG_HIDDEN);
        a_dot(rdots[i], lv_obj_get_width(rdots[i]));
        dot_to(rdots[i], i == r, C_VIOLET);
    }
}




static lv_obj_t *toast, *toast_lbl;
static void toast_create(void)
{
    toast = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(toast);
    lv_obj_set_size(toast, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_pad_hor(toast, 14, 0);
    lv_obj_set_style_radius(toast, 14, 0);
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x111A24), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(toast, 1, 0);
    lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, -40);
    toast_lbl = mk_label(toast, &lv_font_montserrat_14, C_TEXT, "");
    lv_obj_center(toast_lbl);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
}
static void toast_hide_cb(lv_anim_t *a) { (void)a; lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN); }
static void toast_show(const char *txt, uint32_t col)
{
    char b[48];
    snprintf(b, sizeof(b), "%s", txt);
    if (strlen(b) > 30) strcpy(b + 28, "..");
    lv_label_set_text(toast_lbl, b);
    lv_obj_set_style_text_color(toast_lbl, lv_color_hex(col), 0);
    lv_obj_set_style_border_color(toast, lv_color_hex(col), 0);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(toast);
    lv_anim_del(toast, NULL);
    anim(toast, a_y, -40, 34, 350, 0, lv_anim_path_overshoot);
    lv_anim_t an; lv_anim_init(&an);
    lv_anim_set_var(&an, toast); lv_anim_set_exec_cb(&an, a_y);
    lv_anim_set_values(&an, 34, -40); lv_anim_set_time(&an, 300);
    lv_anim_set_delay(&an, 2600); lv_anim_set_path_cb(&an, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&an, toast_hide_cb);
    lv_anim_start(&an);
}




static lv_obj_t *splash;
static void splash_del_cb(lv_anim_t *a) { (void)a; lv_obj_del(splash); splash = NULL; }
static void splash_create(void)
{
    splash = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(splash);
    lv_obj_set_size(splash, SCR, SCR);
    lv_obj_set_style_bg_color(splash, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    lv_obj_clear_flag(splash, LV_OBJ_FLAG_SCROLLABLE);

    const int sz[3] = {224, 196, 168};
    const uint32_t col[3] = {C_CYAN, C_VIOLET, C_GREEN};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *a = lv_arc_create(splash);
        lv_obj_set_size(a, sz[i], sz[i]);
        lv_obj_center(a);
        lv_arc_set_bg_angles(a, 0, 360);
        lv_arc_set_range(a, 0, 360);
        lv_arc_set_value(a, 0);
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_set_style_arc_width(a, 3, LV_PART_MAIN);
        lv_obj_set_style_arc_width(a, 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(a, lv_color_hex(0x0B1118), LV_PART_MAIN);
        lv_obj_set_style_arc_color(a, lv_color_hex(col[i]), LV_PART_INDICATOR);
        anim(a, a_arcv, 0, 360, 900, 150 + i * 160, lv_anim_path_ease_in_out);
        anim(a, a_rot, i & 1 ? 360 : 0, i & 1 ? 0 : 360, 1700, 0, lv_anim_path_ease_out);
    }
    lv_obj_t *n = mk_label(splash, &lv_font_montserrat_14, C_DIM, "NET");
    lv_obj_set_style_text_letter_space(n, 8, 0);
    lv_obj_align(n, LV_ALIGN_CENTER, 4, -22);
    lv_obj_set_style_opa(n, 0, 0);
    anim(n, a_opa, 0, 255, 500, 500, lv_anim_path_linear);

    lv_obj_t *b = mk_label(splash, &lv_font_montserrat_28, C_CYAN, "GHOST");
    lv_obj_set_style_text_letter_space(b, 3, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, 6);
    lv_obj_set_style_opa(b, 0, 0);
    anim(b, a_opa, 0, 255, 400, 750, lv_anim_path_linear);
    anim(b, a_ty, 24, 0, 600, 750, lv_anim_path_overshoot);

    lv_obj_t *v = mk_label(splash, &lv_font_montserrat_12, 0x3B4A5C, "v" FW_VERSION);
    lv_obj_align(v, LV_ALIGN_CENTER, 0, 36);
    lv_obj_set_style_opa(v, 0, 0);
    anim(v, a_opa, 0, 255, 400, 1100, lv_anim_path_linear);

    lv_anim_t an; lv_anim_init(&an);
    lv_anim_set_var(&an, splash); lv_anim_set_exec_cb(&an, a_opa);
    lv_anim_set_values(&an, 255, 0); lv_anim_set_time(&an, 450);
    lv_anim_set_delay(&an, 2500);
    lv_anim_set_ready_cb(&an, splash_del_cb);
    lv_anim_start(&an);
}




static struct {
    lv_obj_t *eth, *wifi, *bt, *alert, *batt, *clock, *ampm, *wx_icon, *wx_lbl, *env, *link;
    char wx_last[16];
} H;

static void draw_wx_icon(const char *cond)
{
    lv_obj_clean(H.wx_icon);
    uint32_t sun = 0xFFD24A, cloud = 0xC7CDD6, rain = 0x5BA8E0;
    bool clear = strstr(cond, "Clear") != NULL;
    bool wet = strstr(cond, "Rain") || strstr(cond, "Drizzle");
    bool storm = strstr(cond, "Thunder") != NULL;
    lv_obj_t *c = lv_obj_create(H.wx_icon);
    lv_obj_remove_style_all(c);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    if (clear) {
        lv_obj_set_size(c, 16, 16); lv_obj_set_style_radius(c, 8, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(sun), 0); lv_obj_center(c);
    } else {
        lv_obj_set_size(c, 20, 11); lv_obj_set_style_radius(c, 6, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(cloud), 0);
        lv_obj_align(c, (wet || storm) ? LV_ALIGN_TOP_MID : LV_ALIGN_CENTER, 0, (wet || storm) ? 3 : 0);
        if (wet || storm) {
            for (int i = 0; i < 3; i++) {
                lv_obj_t *d = lv_obj_create(H.wx_icon);
                lv_obj_remove_style_all(d);
                lv_obj_set_size(d, 2, 5);
                lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(d, lv_color_hex(storm ? sun : rain), 0);
                lv_obj_align(d, LV_ALIGN_BOTTOM_MID, (i - 1) * 6, -2);
            }
        }
    }
}

static void home_create(void)
{
    memset(&H, 0, sizeof(H));
    lv_obj_t *t = page_new(P_HOME, NULL, C_CYAN);

    H.alert = mk_label(t, &lv_font_montserrat_14, C_AMBER, LV_SYMBOL_WARNING);
    lv_obj_align(H.alert, LV_ALIGN_TOP_MID, -62, 27);
    H.eth  = mk_label(t, &lv_font_montserrat_12, C_DIM, "ETH");
    lv_obj_align(H.eth, LV_ALIGN_TOP_MID, -30, 28);
    H.wifi = mk_label(t, &lv_font_montserrat_14, C_DIM, LV_SYMBOL_WIFI);
    lv_obj_align(H.wifi, LV_ALIGN_TOP_MID, 0, 27);
    H.bt   = mk_label(t, &lv_font_montserrat_14, C_DIM, LV_SYMBOL_BLUETOOTH);
    lv_obj_align(H.bt, LV_ALIGN_TOP_MID, 26, 27);
    H.batt = mk_label(t, &lv_font_montserrat_14, C_DIM, LV_SYMBOL_BATTERY_FULL);
    lv_obj_align(H.batt, LV_ALIGN_TOP_MID, 56, 27);

    lv_obj_t *n = center_label(t, &lv_font_montserrat_12, C_DIM, 50, "NET");
    lv_obj_set_style_text_letter_space(n, 7, 0);
    lv_obj_t *b = center_label(t, &lv_font_montserrat_28, C_CYAN, 64, "GHOST");
    lv_obj_set_style_text_letter_space(b, 3, 0);

    H.clock = mk_label(t, &lv_font_montserrat_48, C_TEXT, "--:--");
    lv_obj_align(H.clock, LV_ALIGN_TOP_MID, -10, 96);
    H.ampm = mk_label(t, &lv_font_montserrat_12, C_DIM, "");
    lv_obj_align_to(H.ampm, H.clock, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -10);

    H.wx_icon = lv_obj_create(t);
    lv_obj_remove_style_all(H.wx_icon);
    lv_obj_set_size(H.wx_icon, 24, 24);
    lv_obj_clear_flag(H.wx_icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(H.wx_icon, LV_ALIGN_TOP_MID, -54, 148);
    H.wx_lbl = mk_label(t, &lv_font_montserrat_14, C_TEXT, "Weather --");
    lv_obj_align(H.wx_lbl, LV_ALIGN_TOP_MID, 10, 152);

    H.env  = center_label(t, &lv_font_montserrat_12, C_DIM, 174, "");
    H.link = center_label(t, &lv_font_montserrat_12, C_DIM, 192, "Plug in Ethernet");
    lv_label_set_long_mode(H.link, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(H.link, 150);
}

static void home_update(void)
{
    set_col(H.eth, g.eth_link ? (g.eth_ip ? C_GREEN : C_AMBER) : C_DIM);
    set_col(H.wifi, g.wifi_conn ? C_GREEN : C_DIM);
    set_col(H.bt, g.ble_conn ? C_VIOLET : (g.ble_on ? 0x4C4470 : C_DIM));
    if (g.alert_bad) { lv_obj_clear_flag(H.alert, LV_OBJ_FLAG_HIDDEN); set_col(H.alert, C_AMBER); }
    else lv_obj_add_flag(H.alert, LV_OBJ_FLAG_HIDDEN);
    if (!cfg.battery) lv_obj_add_flag(H.batt, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_clear_flag(H.batt, LV_OBJ_FLAG_HIDDEN);
        const char *sym = !g.batt_present ? LV_SYMBOL_BATTERY_EMPTY : g.batt_charging ? LV_SYMBOL_CHARGE :
                          g.batt_pct > 80 ? LV_SYMBOL_BATTERY_FULL : g.batt_pct > 55 ? LV_SYMBOL_BATTERY_3 :
                          g.batt_pct > 30 ? LV_SYMBOL_BATTERY_2 : g.batt_pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        set_txt(H.batt, sym);
        set_col(H.batt, !g.batt_present ? C_DIM : g.batt_charging ? C_CYAN : g.batt_pct > 30 ? C_GREEN : g.batt_pct > 15 ? C_AMBER : C_RED);
    }

    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    if (tm.tm_year >= (2020 - 1900)) {
        int h12 = (tm.tm_hour % 12 == 0) ? 12 : tm.tm_hour % 12;
        set_fmt(H.clock, "%d:%02d", h12, tm.tm_min);
        set_txt(H.ampm, tm.tm_hour < 12 ? "AM" : "PM");
    } else {
        set_txt(H.clock, "--:--");
        set_txt(H.ampm, "");
    }
    lv_obj_align(H.clock, LV_ALIGN_TOP_MID, -10, 96);
    lv_obj_align_to(H.ampm, H.clock, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -10);

    if (g.wx_valid) {
        set_fmt(H.wx_lbl, "%d\xC2\xB0""F  %s", g.wx_temp, g.wx_cond);
        if (strcmp(H.wx_last, g.wx_cond)) { snprintf(H.wx_last, sizeof(H.wx_last), "%s", g.wx_cond); draw_wx_icon(g.wx_cond); }
    }

    if (g.eth_ip) { set_fmt(H.env, "%s  %dM %s", g.ip, g.eth_speed, g.eth_full ? "FD" : "HD"); set_col(H.env, C_GREEN); }
    else if (g.eth_link) { set_txt(H.env, "Link up, waiting for DHCP"); set_col(H.env, C_AMBER); }
    else { set_txt(H.env, "Plug in Ethernet"); set_col(H.env, C_DIM); }

    char port[40];
    if (g.sw_found) {
        abbr_port(port, sizeof(port), g.sw_port);
        set_fmt(H.link, "%s  " LV_SYMBOL_RIGHT "  VLAN %s", port, g.sw_vlan);
        set_col(H.link, C_ORANGE);
    } else if (g.eth_link) {
        set_txt(H.link, "Listening for switch...");
        set_col(H.link, C_DIM);
    } else {
        set_txt(H.link, "");
    }
}




static const char *test_names[TS_COUNT] = { "LINK", "DHCP", "GATEWAY", "DNS", "INTERNET", "SWITCH" };
static struct {
    lv_obj_t *icon[TS_COUNT], *spin[TS_COUNT], *val[TS_COUNT];
    lv_obj_t *btn_lbl;
    tstate_t  last[TS_COUNT];
} T;

static void run_test_cb(lv_event_t *e) { (void)e; autotest_start(); }

static void test_create(void)
{
    memset(&T, 0, sizeof(T));
    lv_obj_t *t = page_new(P_TEST, "AUTOTEST", C_GREEN);
    for (int i = 0; i < TS_COUNT; i++) {
        int y = 42 + i * 21;
        int w = band_w(y, y + 18, 34);
        if (w > 184) w = 184;
        lv_obj_t *row = lv_obj_create(t);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, w, 18);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        T.icon[i] = mk_label(row, &lv_font_montserrat_14, 0x2A3442, LV_SYMBOL_MINUS);
        lv_obj_align(T.icon[i], LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_transform_pivot_x(T.icon[i], 7, 0);
        lv_obj_set_style_transform_pivot_y(T.icon[i], 8, 0);
        T.spin[i] = lv_spinner_create(row, 900, 70);
        lv_obj_set_size(T.spin[i], 14, 14);
        lv_obj_align(T.spin[i], LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_arc_width(T.spin[i], 2, LV_PART_MAIN);
        lv_obj_set_style_arc_width(T.spin[i], 2, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(T.spin[i], lv_color_hex(0x1A2430), LV_PART_MAIN);
        lv_obj_set_style_arc_color(T.spin[i], lv_color_hex(C_CYAN), LV_PART_INDICATOR);
        lv_obj_add_flag(T.spin[i], LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *n = mk_label(row, &lv_font_montserrat_12, C_DIM, test_names[i]);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 20, 0);

        T.val[i] = mk_label(row, &lv_font_montserrat_14, C_TEXT, "");
        lv_label_set_long_mode(T.val[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(T.val[i], w - 92);
        lv_obj_set_style_text_align(T.val[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(T.val[i], LV_ALIGN_RIGHT_MID, 0, 0);
        T.last[i] = T_IDLE;
    }
    pill_btn(t, 174, 108, "RUN TEST", C_GREEN, run_test_cb, &T.btn_lbl);
}

static void a_zoom(void *o, int32_t v) { lv_obj_set_style_transform_zoom((lv_obj_t *)o, v, 0); }

static void test_update(void)
{
    int done = 0, worst = 0;
    for (int i = 0; i < TS_COUNT; i++) {
        tstep_t *s = &g.ts[i];
        bool run = s->st == T_RUN;
        if (run) lv_obj_clear_flag(T.spin[i], LV_OBJ_FLAG_HIDDEN);
        else     lv_obj_add_flag(T.spin[i], LV_OBJ_FLAG_HIDDEN);
        if (run) lv_obj_add_flag(T.icon[i], LV_OBJ_FLAG_HIDDEN);
        else     lv_obj_clear_flag(T.icon[i], LV_OBJ_FLAG_HIDDEN);

        const char *sym = LV_SYMBOL_MINUS; uint32_t col = 0x2A3442;
        if (s->st == T_PASS) { sym = LV_SYMBOL_OK;      col = C_GREEN; }
        if (s->st == T_WARN) { sym = LV_SYMBOL_WARNING; col = C_AMBER; }
        if (s->st == T_FAIL) { sym = LV_SYMBOL_CLOSE;   col = C_RED; }
        set_txt(T.icon[i], sym);
        set_col(T.icon[i], col);
        if (s->st != T.last[i] && (s->st == T_PASS || s->st == T_WARN || s->st == T_FAIL)) {
            anim(T.icon[i], a_zoom, 40, 256, 450, 0, lv_anim_path_overshoot);
        }
        T.last[i] = s->st;
        set_txt(T.val[i], s->val);
        set_col(T.val[i], s->st == T_FAIL ? C_RED : (s->st == T_WARN ? C_AMBER : C_TEXT));
        if (s->st >= T_PASS) done++;
        if (s->st == T_WARN && worst < 1) worst = 1;
        if (s->st == T_FAIL) worst = 2;
    }
    set_txt(T.btn_lbl, g.test_running ? "TESTING..." : (g.test_runs ? "RUN AGAIN" : "RUN TEST"));
    uint32_t rc = worst == 2 ? C_RED : worst == 1 ? C_AMBER : C_GREEN;
    lv_obj_set_style_arc_color(rings[P_TEST], lv_color_hex(rc), LV_PART_INDICATOR);
    if (!lv_anim_get(rings[P_TEST], a_arcv))
        arc_to(rings[P_TEST], g.test_runs || g.test_running ? done * 360 / TS_COUNT : 360);
}




static struct {
    lv_obj_t *listen, *pulse[3], *wait_lbl, *wait_sub;
    lv_obj_t *found, *proto, *name, *port, *vlan, *voice, *mode, *mgmt;
} S;

static void pulse_loop(lv_obj_t *o, int delay)
{
    if (!cfg.animations) return;
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, o); lv_anim_set_exec_cb(&a, a_size);
    lv_anim_set_values(&a, 20, 150); lv_anim_set_time(&a, 2400);
    lv_anim_set_delay(&a, delay); lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_anim_t b; lv_anim_init(&b);
    lv_anim_set_var(&b, o); lv_anim_set_exec_cb(&b, a_opa);
    lv_anim_set_values(&b, 255, 0); lv_anim_set_time(&b, 2400);
    lv_anim_set_delay(&b, delay); lv_anim_set_repeat_count(&b, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&b);
}

static void switch_create(void)
{
    memset(&S, 0, sizeof(S));
    lv_obj_t *t = page_new(P_SWITCH, "SWITCH", C_ORANGE);


    S.listen = lv_obj_create(t);
    lv_obj_remove_style_all(S.listen);
    lv_obj_set_size(S.listen, 170, 170);
    lv_obj_align(S.listen, LV_ALIGN_CENTER, 0, 8);
    lv_obj_clear_flag(S.listen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *c = lv_obj_create(S.listen);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, 20, 20);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(c, 2, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(C_ORANGE), 0);
        lv_obj_center(c);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
        S.pulse[i] = c;
        pulse_loop(c, i * 800);
    }
    lv_obj_t *dot = lv_obj_create(S.listen);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, 5, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(C_ORANGE), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, -16);
    S.wait_lbl = mk_label(S.listen, &lv_font_montserrat_14, C_TEXT, "LISTENING");
    lv_obj_set_style_text_letter_space(S.wait_lbl, 2, 0);
    lv_obj_align(S.wait_lbl, LV_ALIGN_CENTER, 0, 8);
    S.wait_sub = mk_label(S.listen, &lv_font_montserrat_12, C_DIM, "LLDP  CDP  FDP");
    lv_obj_set_style_text_align(S.wait_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(S.wait_sub, LV_ALIGN_CENTER, 0, 34);


    S.found = lv_obj_create(t);
    lv_obj_remove_style_all(S.found);
    lv_obj_set_size(S.found, SCR, 180);
    lv_obj_align(S.found, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_clear_flag(S.found, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    S.proto = mk_label(S.found, &lv_font_montserrat_12, C_BG, "LLDP");
    lv_obj_set_style_bg_color(S.proto, lv_color_hex(C_ORANGE), 0);
    lv_obj_set_style_bg_opa(S.proto, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(S.proto, 6, 0);
    lv_obj_set_style_pad_hor(S.proto, 6, 0);
    lv_obj_set_style_pad_ver(S.proto, 1, 0);
    lv_obj_align(S.proto, LV_ALIGN_TOP_MID, 0, 0);

    S.name = mk_label(S.found, &lv_font_montserrat_16, C_TEXT, "");
    lv_label_set_long_mode(S.name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(S.name, 176);
    lv_obj_set_style_text_align(S.name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(S.name, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *pl = mk_label(S.found, &lv_font_montserrat_12, C_DIM, "PORT");
    lv_obj_set_style_text_letter_space(pl, 3, 0);
    lv_obj_align(pl, LV_ALIGN_TOP_MID, 0, 44);
    S.port = mk_label(S.found, &lv_font_montserrat_28, C_ORANGE, "");
    lv_label_set_long_mode(S.port, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(S.port, 200);
    lv_obj_set_style_text_align(S.port, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(S.port, LV_ALIGN_TOP_MID, 0, 58);

    kv_row_ex(S.found, 96,  96 + 38,  "VLAN",  &S.vlan);
    kv_row_ex(S.found, 114, 114 + 38, "VOICE", &S.voice);
    kv_row_ex(S.found, 132, 132 + 38, "MODE",  &S.mode);
    kv_row_ex(S.found, 150, 150 + 38, "MGMT",  &S.mgmt);
    lv_obj_add_flag(S.found, LV_OBJ_FLAG_HIDDEN);
}

static void switch_update(void)
{
    if (!g.sw_found) {
        lv_obj_clear_flag(S.listen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(S.found, LV_OBJ_FLAG_HIDDEN);
        if (!g.eth_link) {
            set_txt(S.wait_lbl, "NO LINK");
            set_txt(S.wait_sub, "Plug into a switch port");
        } else {
            set_txt(S.wait_lbl, "LISTENING");
            int64_t s = g.link_up_ms ? (now_ms() - g.link_up_ms) / 1000 : 0;
            set_fmt(S.wait_sub, "LLDP  CDP  FDP\n%ds  (up to 60s)", (int)s);
        }
        return;
    }
    if (lv_obj_has_flag(S.found, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(S.listen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(S.found, LV_OBJ_FLAG_HIDDEN);
        anim(S.found, a_ty, 20, 0, 400, 0, lv_anim_path_overshoot);
    }
    char port[40];
    abbr_port(port, sizeof(port), g.sw_port);
    set_txt(S.proto, g.sw_proto);
    set_txt(S.name, g.sw_name);
    set_txt(S.port, port[0] ? port : "--");
    set_txt(S.vlan, g.sw_vlan[0] ? g.sw_vlan : "--");
    set_txt(S.voice, g.sw_voice[0] ? g.sw_voice : "none");
    set_txt(S.mode, g.sw_mode[0] ? g.sw_mode : "--");
    set_txt(S.mgmt, g.sw_mgmt[0] ? g.sw_mgmt : "--");
}




static struct { lv_obj_t *model, *ver, *caps, *duplex, *poe, *desc, *stp, *age, *none; } D;

static void swdet_create(void)
{
    memset(&D, 0, sizeof(D));
    lv_obj_t *t = page_new(P_SWDET, "SWITCH INFO", C_ORANGE);
    kv_row(t, 42,  "MODEL",  &D.model);
    kv_row(t, 61,  "SW",     &D.ver);
    kv_row(t, 80,  "CAPS",   &D.caps);
    kv_row(t, 99,  "DUPLEX", &D.duplex);
    kv_row(t, 118, "POE",    &D.poe);
    kv_row(t, 137, "DESC",   &D.desc);
    kv_row(t, 156, "STP",    &D.stp);
    kv_row(t, 175, "SEEN",   &D.age);
}

static void swdet_update(void)
{
    char b[48];
    set_txt(D.model, g.sw_model[0] ? g.sw_model : "--");
    set_txt(D.ver, g.sw_ver[0] ? g.sw_ver : "--");
    set_txt(D.caps, g.sw_caps[0] ? g.sw_caps : "--");
    set_txt(D.duplex, g.sw_duplex[0] ? g.sw_duplex : "--");
    set_txt(D.poe, g.sw_poe[0] ? g.sw_poe : "--");
    set_txt(D.desc, g.sw_pdesc[0] ? g.sw_pdesc : "--");
    if (g.stp_seen) set_fmt(D.stp, "%s%s", g.stp_root, g.stp_tc ? " TC!" : "");
    else set_txt(D.stp, "no BPDUs");
    if (g.sw_found) {
        fmt_dur(b, sizeof(b), (now_ms() - g.sw_seen_ms) / 1000);
        set_fmt(D.age, "%s ago  x%u", b, (unsigned)g.sw_frames);
    } else set_txt(D.age, "--");
}




static struct {
    lv_obj_t *speed, *duplex, *chart, *traffic, *bcast, *up;
    lv_chart_series_t *ser;
    lv_obj_t *blink_lbl;
} PS;

static void blink_cb(lv_event_t *e) { (void)e; eth_blink_start(12); }

static void port_create(void)
{
    memset(&PS, 0, sizeof(PS));
    lv_obj_t *t = page_new(P_PORT, "PORT STATS", C_CYAN);
    PS.speed = center_label(t, &lv_font_montserrat_28, C_TEXT, 38, "--");
    PS.duplex = center_label(t, &lv_font_montserrat_12, C_DIM, 70, "NO LINK");
    lv_obj_set_style_text_letter_space(PS.duplex, 2, 0);

    PS.chart = lv_chart_create(t);
    lv_obj_set_size(PS.chart, 172, 40);
    lv_obj_align(PS.chart, LV_ALIGN_TOP_MID, 0, 88);
    lv_chart_set_type(PS.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(PS.chart, 48);
    lv_chart_set_div_line_count(PS.chart, 0, 0);
    lv_chart_set_update_mode(PS.chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_opa(PS.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(PS.chart, 0, 0);
    lv_obj_set_style_pad_all(PS.chart, 0, 0);
    lv_obj_set_style_size(PS.chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(PS.chart, 2, LV_PART_ITEMS);
    lv_obj_clear_flag(PS.chart, LV_OBJ_FLAG_CLICKABLE);
    PS.ser = lv_chart_add_series(PS.chart, lv_color_hex(C_CYAN), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_range(PS.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);

    kv_row(t, 132, "TRAFFIC", &PS.traffic);
    kv_row(t, 150, "BC / MC", &PS.bcast);
    kv_row(t, 168, "LINK UP", &PS.up);
    pill_btn(t, 189, 112, "BLINK PORT", C_CYAN, blink_cb, &PS.blink_lbl);
}

static void port_update(void)
{
    char b[24], c[24];
    if (g.eth_link) {
        set_fmt(PS.speed, "%d Mb/s", g.eth_speed ? g.eth_speed : 100);
        set_txt(PS.duplex, g.eth_full ? "FULL DUPLEX" : "HALF DUPLEX");
        set_col(PS.duplex, g.eth_full ? C_GREEN : C_AMBER);
    } else {
        set_txt(PS.speed, "--");
        set_txt(PS.duplex, "NO LINK");
        set_col(PS.duplex, C_RED);
    }
    uint16_t mx = 10;
    for (int i = 0; i < 48; i++) if (g.fps_hist[i] > mx) mx = g.fps_hist[i];
    lv_chart_set_range(PS.chart, LV_CHART_AXIS_PRIMARY_Y, 0, mx + mx / 5);
    for (int i = 0; i < 48; i++) PS.ser->y_points[i] = g.fps_hist[i];
    lv_chart_refresh(PS.chart);

    fmt_count(c, sizeof(c), g.bps);
    set_fmt(PS.traffic, "%u f/s  %sB/s", (unsigned)g.fps, c);
    set_fmt(PS.bcast, "%u%% / %u%%", (unsigned)g.bc_pct, (unsigned)g.mc_pct);
    set_col(PS.bcast, g.bc_pct > 30 && g.fps > 50 ? C_RED : C_TEXT);
    if (g.eth_link && g.link_up_ms) {
        fmt_dur(b, sizeof(b), (now_ms() - g.link_up_ms) / 1000);
        set_fmt(PS.up, "%s  %u flap%s", b, (unsigned)g.link_flaps, g.link_flaps == 1 ? "" : "s");
    } else set_fmt(PS.up, "down  %u flaps", (unsigned)g.link_flaps);
    set_txt(PS.blink_lbl, g.blinking ? "BLINKING..." : "BLINK PORT");
}




static struct { lv_obj_t *link, *ip, *mask, *gw, *dns, *mac, *srv, *lease; } N;

static void ip_renew_cb(lv_event_t *e) { (void)e; eth_dhcp_renew(); toast_show(LV_SYMBOL_REFRESH "  Renewing DHCP", C_BLUE); }
static void ip_create(void)
{
    memset(&N, 0, sizeof(N));
    lv_obj_t *t = page_new(P_IP, "IP CONFIG", C_VIOLET);
    kv_row(t, 42,  "LINK",  &N.link);
    kv_row(t, 61,  "IP",    &N.ip);
    kv_row(t, 80,  "MASK",  &N.mask);
    kv_row(t, 99,  "GW",    &N.gw);
    kv_row(t, 118, "DNS",   &N.dns);
    kv_row(t, 137, "MAC",   &N.mac);
    kv_row(t, 156, "DHCP",  &N.srv);
    kv_row(t, 168, "LEASE", &N.lease);
    pill_btn(t, 190, 104, "RENEW DHCP", C_BLUE, ip_renew_cb, NULL);
}

static void ip_update(void)
{
    char b[24];
    if (!g.eth_link) { set_txt(N.link, "DOWN"); set_col(N.link, C_RED); }
    else if (!g.eth_ip) { set_txt(N.link, "UP (no IP)"); set_col(N.link, C_AMBER); }
    else { set_fmt(N.link, "UP %dM %s", g.eth_speed, g.eth_full ? "FD" : "HD"); set_col(N.link, C_GREEN); }
    set_txt(N.ip, g.eth_ip ? g.ip : "---");
    set_txt(N.mask, g.eth_ip ? g.mask : "---");
    set_txt(N.gw, g.eth_ip ? g.gw : "---");
    set_txt(N.dns, g.eth_ip ? g.dns : "---");
    set_txt(N.mac, g.mac);
    if (g.eth_ip) set_fmt(N.srv, "%s  %.1fs", g.dhcp_srv[0] ? g.dhcp_srv : "?", g.dhcp_ms / 1000.0f);
    else set_txt(N.srv, "---");
    if (g.lease_s) { fmt_dur(b, sizeof(b), g.lease_s); set_txt(N.lease, b); }
    else set_txt(N.lease, "---");
}




static struct { lv_obj_t *arc, *dbm, *qual, *ssid, *ip, *chan, *bssid, *off; } W;

static void wifi_create(void)
{
    memset(&W, 0, sizeof(W));
    lv_obj_t *t = page_new(P_WIFI, "WI-FI", C_GREEN);
    W.arc = lv_arc_create(t);
    lv_obj_set_size(W.arc, 104, 104);
    lv_obj_align(W.arc, LV_ALIGN_TOP_MID, 0, 38);
    lv_arc_set_rotation(W.arc, 135);
    lv_arc_set_bg_angles(W.arc, 0, 270);
    lv_arc_set_range(W.arc, -100, -30);
    lv_arc_set_value(W.arc, -100);
    lv_obj_remove_style(W.arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(W.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(W.arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(W.arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(W.arc, lv_color_hex(0x16202B), LV_PART_MAIN);
    lv_obj_set_style_arc_color(W.arc, lv_color_hex(C_GREEN), LV_PART_INDICATOR);
    W.dbm = mk_label(W.arc, &lv_font_montserrat_20, C_TEXT, "--");
    lv_obj_align(W.dbm, LV_ALIGN_CENTER, 0, -6);
    W.qual = mk_label(W.arc, &lv_font_montserrat_12, C_DIM, "dBm");
    lv_obj_align(W.qual, LV_ALIGN_CENTER, 0, 16);

    kv_row(t, 146, "SSID", &W.ssid);
    kv_row(t, 164, "IP",   &W.ip);
    kv_row(t, 182, "CH",   &W.chan);
    W.bssid = center_label(t, &lv_font_montserrat_12, C_DIM, 202, "");
}

static void wifi_update(void)
{
    if (g.wifi_conn) {
        arc_to(W.arc, g.w_rssi < -100 ? -100 : (g.w_rssi > -30 ? -30 : g.w_rssi));
        uint32_t c = g.w_rssi >= -67 ? C_GREEN : (g.w_rssi >= -75 ? C_AMBER : C_RED);
        lv_obj_set_style_arc_color(W.arc, lv_color_hex(c), LV_PART_INDICATOR);
        set_fmt(W.dbm, "%d", g.w_rssi);
        set_fmt(W.qual, "dBm  %s", rssi_quality(g.w_rssi));
        set_txt(W.ssid, g.w_ssid);
        set_txt(W.ip, g.w_ip);
        set_fmt(W.chan, "%d  %s  %s", g.w_chan, g.w_phy, g.w_auth);
        set_txt(W.bssid, g.w_bssid);
    } else {
        arc_to(W.arc, -100);
        set_txt(W.dbm, "--");
        set_txt(W.qual, "offline");
        set_txt(W.ssid, g.w_cfg_ssid[0] ? g.w_cfg_ssid : "not set");
        set_txt(W.ip, "---");
        set_txt(W.chan, g.w_cfg_ssid[0] ? "connecting" : "set from GUI");
        set_txt(W.bssid, "");
    }
}




#define SCAN_ROWS 5
static struct {
    lv_obj_t *hdr, *sym[SCAN_ROWS], *ssid[SCAN_ROWS], *meta[SCAN_ROWS];
    lv_obj_t *chart, *btn_lbl, *sweep;
    lv_chart_series_t *ser;
    bool sweeping;
} SC;

static void scan_cb(lv_event_t *e) { (void)e; wifi_scan_start(); }

static void scan_create(void)
{
    memset(&SC, 0, sizeof(SC));
    lv_obj_t *t = page_new(P_SCAN, "WI-FI SCAN", C_GREEN);
    SC.hdr = center_label(t, &lv_font_montserrat_12, C_DIM, 38, "scanning...");
    for (int i = 0; i < SCAN_ROWS; i++) {
        int y = 54 + i * 18;
        int w = band_w(y, y + 17, 30); if (w > 184) w = 184;
        lv_obj_t *row = lv_obj_create(t);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, w, 18);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        SC.sym[i] = mk_label(row, &lv_font_montserrat_12, C_DIM, LV_SYMBOL_WIFI);
        lv_obj_align(SC.sym[i], LV_ALIGN_LEFT_MID, 0, 0);
        SC.ssid[i] = mk_label(row, &lv_font_montserrat_14, C_TEXT, "");
        lv_label_set_long_mode(SC.ssid[i], LV_LABEL_LONG_DOT);
        lv_obj_set_size(SC.ssid[i], w - 80, 17);
        lv_obj_align(SC.ssid[i], LV_ALIGN_LEFT_MID, 18, 0);
        SC.meta[i] = mk_label(row, &lv_font_montserrat_12, C_DIM, "");
        lv_obj_align(SC.meta[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }
    SC.chart = lv_chart_create(t);
    lv_obj_set_size(SC.chart, 150, 30);
    lv_obj_align(SC.chart, LV_ALIGN_TOP_MID, 0, 148);
    lv_chart_set_type(SC.chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(SC.chart, 13);
    lv_chart_set_div_line_count(SC.chart, 0, 0);
    lv_obj_set_style_bg_opa(SC.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(SC.chart, 0, 0);
    lv_obj_set_style_pad_all(SC.chart, 0, 0);
    lv_obj_set_style_pad_column(SC.chart, 3, 0);
    lv_obj_set_style_radius(SC.chart, 2, LV_PART_ITEMS);
    lv_obj_clear_flag(SC.chart, LV_OBJ_FLAG_CLICKABLE);
    SC.ser = lv_chart_add_series(SC.chart, lv_color_hex(C_GREEN), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_range(SC.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 4);
    lv_obj_t *cl = center_label(t, &lv_font_montserrat_12, 0x3B4A5C, 178, "CH 1  -  CHANNEL LOAD  -  13");
    (void)cl;
    pill_btn(t, 196, 80, "SCAN", C_GREEN, scan_cb, &SC.btn_lbl);
}

static void scan_update(void)
{

    if (g.scanning && cfg.animations && !SC.sweeping) {
        SC.sweeping = true;
        lv_anim_del(rings[P_SCAN], a_arcv);
        lv_arc_set_value(rings[P_SCAN], 70);
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, rings[P_SCAN]); lv_anim_set_exec_cb(&a, a_rot);
        lv_anim_set_values(&a, 0, 360); lv_anim_set_time(&a, 1100);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);
    } else if ((!g.scanning || !cfg.animations) && SC.sweeping) {
        SC.sweeping = false;
        lv_anim_del(rings[P_SCAN], a_rot);
        lv_arc_set_rotation(rings[P_SCAN], 270);
        anim(rings[P_SCAN], a_arcv, 70, 360, 500, 0, NULL);
    }
    set_txt(SC.btn_lbl, g.scanning ? "..." : "SCAN");

    bst_lock();
    int n = g.ap_count;
    if (n == 0) set_txt(SC.hdr, g.scanning ? "scanning..." : "no networks yet");
    else {
        int best = 1, bc = 999;
        static const int good[3] = {1, 6, 11};
        for (int i = 0; i < 3; i++) if (g.chan_cnt[good[i]] < bc) { bc = g.chan_cnt[good[i]]; best = good[i]; }
        set_fmt(SC.hdr, "%d NETWORKS  " LV_SYMBOL_RIGHT "  BEST CH %d", n, best);
    }
    for (int i = 0; i < SCAN_ROWS; i++) {
        if (i < n) {
            ap_rec_t *a = &g.aps[i];
            set_txt(SC.ssid[i], a->ssid[0] ? a->ssid : "(hidden)");
            set_fmt(SC.meta[i], "%d  %d", a->chan, a->rssi);
            set_col(SC.sym[i], a->rssi >= -60 ? C_GREEN : a->rssi >= -72 ? C_AMBER : C_RED);
            set_col(SC.ssid[i], strcmp(a->auth, "OPEN") == 0 ? C_AMBER : C_TEXT);
        } else {
            set_txt(SC.ssid[i], ""); set_txt(SC.meta[i], ""); set_col(SC.sym[i], 0x1A2430);
        }
    }
    int mx = 4;
    for (int c = 1; c <= 13; c++) if (g.chan_cnt[c] > mx) mx = g.chan_cnt[c];
    for (int c = 1; c <= 13; c++) SC.ser->y_points[c - 1] = g.chan_cnt[c];
    bst_unlock();
    lv_chart_set_range(SC.chart, LV_CHART_AXIS_PRIMARY_Y, 0, mx);
    lv_chart_refresh(SC.chart);
}




static struct { lv_obj_t *up, *temp, *heap, *batt, *rst, *ble, *ap, *mqtt; } DV;

static void dev_create(void)
{
    memset(&DV, 0, sizeof(DV));
    lv_obj_t *t = page_new(P_DEV, "DEVICE", C_BLUE);
    kv_row(t, 42,  "UPTIME",  &DV.up);
    kv_row(t, 61,  "CHIP",    &DV.temp);
    kv_row(t, 80,  "MEMORY",  &DV.heap);
    kv_row(t, 99,  "BATTERY", &DV.batt);
    kv_row(t, 118, "RESET",   &DV.rst);
    kv_row(t, 137, "BLE",     &DV.ble);
    kv_row(t, 156, "HOTSPOT", &DV.ap);
    kv_row(t, 175, "MQTT",    &DV.mqtt);
}

static void dev_update(void)
{
    char b[24];
    fmt_dur(b, sizeof(b), (now_ms() - g.boot_us / 1000) / 1000);
    set_txt(DV.up, b);
    set_fmt(DV.temp, "%.1f\xC2\xB0%s", cfg.units_c ? g.chip_temp : c2f(g.chip_temp), cfg.units_c ? "C" : "F");
    set_fmt(DV.heap, "%uk free  %uk min", (unsigned)(g.heap_free / 1024), (unsigned)(g.heap_min / 1024));
    set_col(DV.heap, g.heap_min < 20000 ? C_AMBER : C_TEXT);
    if (!cfg.battery) set_txt(DV.batt, "not fitted");
    else if (!g.batt_present) set_txt(DV.batt, "no reading");
    else set_fmt(DV.batt, "%d%%  %.2fV%s", g.batt_pct, g.batt_v, g.batt_charging ? "  chg" : "");
    set_col(DV.batt, g.batt_present && g.batt_pct <= 15 && !g.batt_charging ? C_RED : C_TEXT);
    set_txt(DV.rst, g.reset_reason);
    set_txt(DV.ble, g.ble_conn ? "phone connected" : (g.ble_on ? "advertising" : "off"));
    set_col(DV.ble, g.ble_conn ? C_VIOLET : C_TEXT);
    set_fmt(DV.ap, "%d client%s", g.ap_clients, g.ap_clients == 1 ? "" : "s");
    set_txt(DV.mqtt, g.mqtt_conn ? "connected" : "offline");
    set_col(DV.mqtt, g.mqtt_conn ? C_GREEN : C_DIM);
}







static struct { lv_obj_t *src, *batt, *volt, *chg, *usb, *poe, *note; } PW;
static void power_create(void)
{
    memset(&PW, 0, sizeof(PW));
    lv_obj_t *t = page_new(P_POWER, "POWER", C_GREEN);
    kv_row(t, 44,  "SOURCE",   &PW.src);
    kv_row(t, 64,  "BATTERY",  &PW.batt);
    kv_row(t, 84,  "VOLTAGE",  &PW.volt);
    kv_row(t, 104, "CHARGING", &PW.chg);
    kv_row(t, 124, "USB-C",    &PW.usb);
    kv_row(t, 144, "PoE",      &PW.poe);
    PW.note = center_label(t, &lv_font_montserrat_12, C_DIM, 180, "USB-C watts: no meter on board");
    lv_obj_set_width(PW.note, 180); lv_label_set_long_mode(PW.note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(PW.note, LV_TEXT_ALIGN_CENTER, 0);
}
static void power_update(void)
{
    bool batt = cfg.battery && g.batt_present;
    if (batt && g.batt_charging) { set_txt(PW.src, "USB-C (charging)"); set_col(PW.src, C_GREEN); }
    else if (batt) { set_txt(PW.src, "Battery"); set_col(PW.src, g.batt_pct <= 15 ? C_RED : C_AMBER); }
    else { set_txt(PW.src, "USB-C / PoE"); set_col(PW.src, C_TEXT); }
    if (!cfg.battery) set_txt(PW.batt, "not fitted");
    else if (!g.batt_present) set_txt(PW.batt, "no reading");
    else set_fmt(PW.batt, "%d%%", g.batt_pct);
    set_col(PW.batt, batt && g.batt_pct <= 15 && !g.batt_charging ? C_RED : C_TEXT);
    if (batt) set_fmt(PW.volt, "%.2f V", g.batt_v); else set_txt(PW.volt, "--");
    set_txt(PW.chg, !cfg.battery ? "--" : g.batt_charging ? "yes" : "no");
    set_col(PW.chg, g.batt_charging ? C_GREEN : C_TEXT);
    set_txt(PW.usb, "not metered");

    if (poe_is_fresh() && g.poe.allocated_mw > 0) set_fmt(PW.poe, "%.1f W adv", g.poe.allocated_mw / 1000.0f);
    else if (g.eth_link) set_txt(PW.poe, "no PoE info");
    else set_txt(PW.poe, "--");
    set_txt(PW.note, batt ? "USB-C watts: no meter on board" : "Running on USB-C or PoE (not metered)");
}




static struct { lv_obj_t *qr, *ssid, *pass, *bt, *hint; char last[96]; } CN;

static void conn_create(void)
{
    memset(&CN, 0, sizeof(CN));
    lv_obj_t *t = page_new(P_CONN, "CONNECT", C_VIOLET);
    CN.bt = mk_label(t, &lv_font_montserrat_14, C_DIM, LV_SYMBOL_BLUETOOTH);
    lv_obj_align(CN.bt, LV_ALIGN_TOP_MID, 66, 19);
    CN.hint = center_label(t, &lv_font_montserrat_12, C_DIM, 40, "SCAN FOR GUI");
    lv_obj_set_style_text_letter_space(CN.hint, 1, 0);


    lv_obj_t *frame = lv_obj_create(t);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, 116, 116);
    lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(frame, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(frame, 8, 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    CN.qr = lv_qrcode_create(frame, 104, lv_color_hex(0x000000), lv_color_hex(0xFFFFFF));
    lv_obj_center(CN.qr);

    CN.ssid = center_label(t, &lv_font_montserrat_12, C_TEXT, 182, "");
    CN.pass = center_label(t, &lv_font_montserrat_12, C_DIM, 198, "");
}

static void conn_update(void)
{
    char q[96];
    snprintf(q, sizeof(q), "WIFI:T:WPA;S:%s;P:%s;;", g.ap_ssid, g.ap_pass);
    if (g.ap_ssid[0] && strcmp(q, CN.last)) {
        snprintf(CN.last, sizeof(CN.last), "%s", q);
        lv_qrcode_update(CN.qr, q, strlen(q));
    }
    set_txt(CN.ssid, g.ap_ssid);
    set_fmt(CN.pass, "PW %s", g.ap_pass);
    set_col(CN.bt, g.ble_conn ? C_VIOLET : (g.ble_on ? 0x4C4470 : C_DIM));
    if (g.ap_clients > 0) set_fmt(CN.hint, "%d PHONE%s ON HOTSPOT", g.ap_clients, g.ap_clients > 1 ? "S" : "");
    else set_txt(CN.hint, g.ble_conn ? "PHONE LINKED OVER BT" : "SCAN FOR GUI");
    set_col(CN.hint, (g.ap_clients || g.ble_conn) ? C_VIOLET : C_DIM);
}




static struct { lv_obj_t *status, *sub, *speed, *sw, *can, *drops, *loss, *lat, *btn_lbl; } CB;

static void quality_cb(lv_event_t *e) { (void)e; quality_start(); }

static void cable_create(void)
{
    memset(&CB, 0, sizeof(CB));
    lv_obj_t *t = page_new(P_CABLE, "CABLE & LINK", C_CYAN);
    CB.status = center_label(t, &lv_font_montserrat_16, C_DIM, 38, "NO LINK");
    lv_obj_set_style_text_letter_space(CB.status, 1, 0);
    kv_row(t, 62,  "SPEED",     &CB.speed);
    kv_row(t, 80,  "SWITCH",    &CB.sw);
    kv_row(t, 98,  "PORT CAN",  &CB.can);
    kv_row(t, 116, "DROPS",     &CB.drops);
    kv_row(t, 134, "LOSS",      &CB.loss);
    kv_row(t, 152, "LATENCY",   &CB.lat);
    pill_btn(t, 176, 112, "TEST LINK", C_CYAN, quality_cb, &CB.btn_lbl);
}

static void cable_update(void)
{
    const char *st = "LINK GOOD"; uint32_t col = C_GREEN;
    int recent = 0;
    for (int i = 0; i < 4; i++) if (g.flap_ms[i] && now_ms() - g.flap_ms[i] < 600000) recent++;
    bool mismatch = false;
    if (g.sw_duplex[0]) {
        bool sf = strstr(g.sw_duplex, "Full"), sh = strstr(g.sw_duplex, "Half");
        mismatch = (sf && !g.eth_full) || (sh && g.eth_full);
    }
    int loss = g.q_sent ? (g.q_sent - g.q_recv) * 100 / g.q_sent : 0;
    if (!g.eth_hw) { st = "NO ETH CHIP"; col = C_RED; }
    else if (!g.eth_link) { st = "NO LINK"; col = C_RED; }
    else if (mismatch) { st = "DUPLEX MISMATCH"; col = C_RED; }
    else if (recent >= 3) { st = "LINK UNSTABLE"; col = C_AMBER; }
    else if (g.eth_speed == 10 || !g.eth_full) { st = "SLOW LINK"; col = C_AMBER; }
    else if (g.q_sent && !g.q_running && loss > 1) { st = "PACKET LOSS"; col = C_AMBER; }
    set_txt(CB.status, st);
    set_col(CB.status, col);
    lv_obj_set_style_arc_color(rings[P_CABLE], lv_color_hex(col), LV_PART_INDICATOR);

    if (g.eth_link) set_fmt(CB.speed, "%dM %s", g.eth_speed, g.eth_full ? "Full" : "Half");
    else set_txt(CB.speed, "--");
    set_txt(CB.sw, g.sw_duplex[0] ? g.sw_duplex : "--");
    set_col(CB.sw, mismatch ? C_RED : C_TEXT);
    set_txt(CB.can, g.sw_adv[0] ? g.sw_adv : "--");
    set_fmt(CB.drops, "%u total  %d in 10m", (unsigned)g.link_flaps, recent);
    set_col(CB.drops, recent >= 3 ? C_AMBER : C_TEXT);
    if (g.q_running) {
        set_fmt(CB.loss, "testing %d%%", g.q_done);
        set_txt(CB.lat, g.q_recv ? "" : "--");
        if (g.q_recv) set_fmt(CB.lat, "%d ms  jit %d", g.q_avg, g.q_jitter);
    } else if (g.q_sent) {
        set_fmt(CB.loss, "%d%%  (%d/%d)", loss, g.q_recv, g.q_sent);
        set_col(CB.loss, loss > 1 ? C_AMBER : C_TEXT);
        set_fmt(CB.lat, "%d ms  jit %d", g.q_avg, g.q_jitter);
    } else { set_txt(CB.loss, "tap TEST LINK"); set_txt(CB.lat, "--"); }
    set_txt(CB.btn_lbl, g.q_running ? "TESTING..." : "TEST LINK");
}




#define PKT_ROWS 5
static struct { lv_obj_t *hdr, *bar[PKT_ROWS], *pct[PKT_ROWS], *apps1, *apps2, *top; } PK;
static const char *pkt_names[PKT_ROWS] = { "IPv4", "IPv6", "ARP", "L2 CTRL", "OTHER" };
static const uint32_t pkt_cols[PKT_ROWS] = { C_CYAN, C_VIOLET, C_AMBER, C_ORANGE, C_DIM };

static void pkt_create(void)
{
    memset(&PK, 0, sizeof(PK));
    lv_obj_t *t = page_new(P_PKT, "PACKETS", C_PINK);
    PK.hdr = center_label(t, &lv_font_montserrat_12, C_DIM, 38, "");
    for (int i = 0; i < PKT_ROWS; i++) {
        int y = 56 + i * 17;
        int w = band_w(y, y + 15, 34); if (w > 180) w = 180;
        lv_obj_t *row = lv_obj_create(t);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, w, 16);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *n = mk_label(row, &lv_font_montserrat_12, C_DIM, pkt_names[i]);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *b = lv_bar_create(row);
        lv_obj_set_size(b, w - 98, 7);
        lv_obj_align(b, LV_ALIGN_LEFT_MID, 58, 0);
        lv_bar_set_range(b, 0, 100);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x16202B), LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_hex(pkt_cols[i]), LV_PART_INDICATOR);
        lv_obj_set_style_anim_time(b, 500, 0);
        PK.bar[i] = b;
        PK.pct[i] = mk_label(row, &lv_font_montserrat_12, C_TEXT, "0%");
        lv_obj_align(PK.pct[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }
    PK.apps1 = center_label(t, &lv_font_montserrat_12, C_TEXT, 146, "");
    PK.apps2 = center_label(t, &lv_font_montserrat_12, C_TEXT, 162, "");
    lv_obj_t *tl = center_label(t, &lv_font_montserrat_12, C_DIM, 180, "TOP TALKER");
    lv_obj_set_style_text_letter_space(tl, 2, 0);
    PK.top = center_label(t, &lv_font_montserrat_14, C_PINK, 194, "--");
    lv_label_set_long_mode(PK.top, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(PK.top, 140);
}

static void pkt_update(void)
{
    char b[24];
    fmt_count(b, sizeof(b), g.rx_frames);
    set_fmt(PK.hdr, "%u f/s   %s seen", (unsigned)g.fps, b);
    uint32_t v[PKT_ROWS] = { g.pk[PK_IPV4], g.pk[PK_IPV6], g.pk[PK_ARP],
                             g.pk[PK_LLDP] + g.pk[PK_CDP] + g.pk[PK_STP], g.pk[PK_OTHER] };
    uint32_t tot = 0;
    for (int i = 0; i < PKT_ROWS; i++) tot += v[i];
    for (int i = 0; i < PKT_ROWS; i++) {
        int p = tot ? (int)(v[i] * 100 / tot) : 0;
        if (lv_bar_get_value(PK.bar[i]) != p) lv_bar_set_value(PK.bar[i], p, LV_ANIM_ON);
        set_fmt(PK.pct[i], "%d%%", p);
    }
    set_fmt(PK.apps1, "DHCP %u   DNS %u   mDNS %u", (unsigned)g.pk[PK_DHCP], (unsigned)g.pk[PK_DNS], (unsigned)g.pk[PK_MDNS]);
    set_fmt(PK.apps2, "SSDP %u   NBNS %u   STP %u", (unsigned)g.pk[PK_SSDP], (unsigned)g.pk[PK_NBNS], (unsigned)g.pk[PK_STP]);
    char tt[40]; pkt_top_talker(tt, sizeof(tt));
    set_txt(PK.top, tt);
}




#define AL_ROWS 6
static struct { lv_obj_t *clear, *clear_sub, *icon[AL_ROWS], *txt[AL_ROWS]; } AL;

static void alert_create(void)
{
    memset(&AL, 0, sizeof(AL));
    lv_obj_t *t = page_new(P_ALERT, "ALERTS", C_AMBER);
    AL.clear = center_label(t, &lv_font_montserrat_28, C_GREEN, 76, LV_SYMBOL_OK);
    AL.clear_sub = center_label(t, &lv_font_montserrat_14, C_TEXT, 116, "ALL CLEAR");
    lv_obj_set_style_text_letter_space(AL.clear_sub, 2, 0);
    for (int i = 0; i < AL_ROWS; i++) {
        int y = 42 + i * 24;
        int w = band_w(y, y + 18, 36); if (w > 184) w = 184;
        lv_obj_t *row = lv_obj_create(t);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, w, 20);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        AL.icon[i] = mk_label(row, &lv_font_montserrat_12, C_AMBER, LV_SYMBOL_WARNING);
        lv_obj_align(AL.icon[i], LV_ALIGN_LEFT_MID, 0, 0);
        AL.txt[i] = mk_label(row, &lv_font_montserrat_12, C_TEXT, "");
        lv_label_set_long_mode(AL.txt[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(AL.txt[i], w - 20);
        lv_obj_align(AL.txt[i], LV_ALIGN_LEFT_MID, 18, 0);
    }
}

static void alert_update(void)
{
    bst_lock();
    int n = g.alert_n;
    bool none = n == 0;
    if (none) { lv_obj_clear_flag(AL.clear, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(AL.clear_sub, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(AL.clear, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(AL.clear_sub, LV_OBJ_FLAG_HIDDEN); }
    int worst = 0;
    for (int i = 0; i < AL_ROWS; i++) {
        if (i < n) {
            alert_t *a = &g.alerts[i];
            if (a->lvl > worst) worst = a->lvl;
            set_txt(AL.icon[i], a->lvl == 2 ? LV_SYMBOL_CLOSE : a->lvl == 1 ? LV_SYMBOL_WARNING : LV_SYMBOL_EYE_OPEN);
            set_col(AL.icon[i], a->lvl == 2 ? C_RED : a->lvl == 1 ? C_AMBER : C_BLUE);
            set_txt(AL.txt[i], a->txt);
            set_col(AL.txt[i], a->lvl == 2 ? C_RED : C_TEXT);
        } else { set_txt(AL.icon[i], ""); set_txt(AL.txt[i], ""); }
    }
    bst_unlock();
    lv_obj_set_style_arc_color(rings[P_ALERT], lv_color_hex(none ? C_GREEN : worst == 2 ? C_RED : worst == 1 ? C_AMBER : C_BLUE), LV_PART_INDICATOR);
}




enum { S_BRIGHT, S_SLEEP, S_UNITS, S_AUTO, S_BATT, S_HOT, S_ROWS };
static const char *set_names[S_ROWS] = { "BRIGHT", "SLEEP", "UNITS", "AUTOTEST", "BATTERY", "HOTSPOT" };
static lv_obj_t *set_val[S_ROWS];

static void set_tap_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    char v[8]; int f = 0;
    switch (i) {
    case S_BRIGHT: snprintf(v, sizeof(v), "%d", cfg.bright >= 100 ? 20 : (cfg.bright / 20 + 1) * 20); f = cfg_set("bright", v); break;
    case S_SLEEP: {
        static const int opts[] = { 0, 30, 60, 300, 600 };
        int k = 0; for (int j = 0; j < 5; j++) if (opts[j] == cfg.sleep_s) k = j;
        snprintf(v, sizeof(v), "%d", opts[(k + 1) % 5]); f = cfg_set("sleep", v); break; }
    case S_UNITS: f = cfg_set("units_c", cfg.units_c ? "0" : "1"); break;
    case S_AUTO:  f = cfg_set("autotest", cfg.autotest ? "0" : "1"); break;
    case S_BATT:  f = cfg_set("battery", cfg.battery ? "0" : "1"); break;
    case S_HOT:   f = cfg_set("hotspot", cfg.hotspot ? "0" : "1"); break;
    }
    cfg_apply(f);
}
static void restart_cb(lv_event_t *e) { (void)e; device_reboot(); }

static void set_create(void)
{
    lv_obj_t *t = page_new(P_SET, "SETTINGS", C_BLUE);
    for (int i = 0; i < S_ROWS; i++) {
        int y = 42 + i * 23;
        int w = band_w(y, y + 19, 40); if (w > 170) w = 170;
        lv_obj_t *row = lv_obj_create(t);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, w, 19);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *k = mk_label(row, &lv_font_montserrat_12, C_DIM, set_names[i]);
        lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *b = lv_btn_create(row);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 64, 18);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x16222E), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x24405A), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x2A3E54), 0);
        lv_obj_add_event_cb(b, set_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        set_val[i] = mk_label(b, &lv_font_montserrat_12, C_TEXT, "");
        lv_obj_center(set_val[i]);
    }
    pill_btn(t, 186, 92, "RESTART", C_RED, restart_cb, NULL);
}

static void set_update(void)
{
    set_fmt(set_val[S_BRIGHT], "%d%%", cfg.bright);
    if (!cfg.sleep_s) set_txt(set_val[S_SLEEP], "never");
    else if (cfg.sleep_s < 60) set_fmt(set_val[S_SLEEP], "%ds", cfg.sleep_s);
    else set_fmt(set_val[S_SLEEP], "%d min", cfg.sleep_s / 60);
    set_txt(set_val[S_UNITS], cfg.units_c ? "\xC2\xB0""C" : "\xC2\xB0""F");
    set_txt(set_val[S_AUTO], cfg.autotest ? "on plug" : "manual");
    set_txt(set_val[S_BATT], cfg.battery ? "fitted" : "none");
    set_txt(set_val[S_HOT], cfg.hotspot ? "on" : "off");
    set_col(set_val[S_HOT], cfg.hotspot ? C_GREEN : C_AMBER);
}


static struct { lv_obj_t *chan, *width, *phy, *sec, *ap, *up, *drops, *reason; } WI;
static void winfo_create(void)
{
    memset(&WI, 0, sizeof(WI));
    lv_obj_t *t = page_new(P_WINFO, "WI-FI LINK", C_CYAN);
    kv_row(t, 42,  "CH", &WI.chan);
    kv_row(t, 61,  "WIDTH",   &WI.width);
    kv_row(t, 80,  "PHY",     &WI.phy);
    kv_row(t, 99,  "SECURITY", &WI.sec);
    kv_row(t, 118, "AP",      &WI.ap);
    kv_row(t, 137, "UP",      &WI.up);
    kv_row(t, 156, "DROPS",   &WI.drops);
    kv_row(t, 175, "LAST WHY", &WI.reason);
}
static void winfo_update(void)
{
    bool c = g.wifi_conn;
    if (c) set_fmt(WI.chan, "%d  %d MHz", g.w_chan, g.w_freq); else set_txt(WI.chan, "offline");
    if (c) set_fmt(WI.width, "%d MHz", g.w_width ? g.w_width : 20); else set_txt(WI.width, "--");
    set_txt(WI.phy, c ? g.w_phy : "--");
    if (c) set_fmt(WI.sec, "%s %s", g.w_auth, g.w_pairwise); else set_txt(WI.sec, "--");
    set_txt(WI.ap, c ? g.w_bssid : "--");
    char b[24];
    if (c && g.w_connected_ms) { fmt_dur(b, sizeof(b), (now_ms() - g.w_connected_ms) / 1000); set_txt(WI.up, b); }
    else set_txt(WI.up, "--");
    set_fmt(WI.drops, "%d", g.w_reconnects);
    set_col(WI.drops, g.w_reconnects > 3 ? C_AMBER : C_TEXT);
    if (g.w_disconnect_reason) set_fmt(WI.reason, "code %d", g.w_disconnect_reason); else set_txt(WI.reason, "none");
}

static struct { lv_obj_t *ip, *mask, *gw, *dns, *mac, *country, *aps, *ssid; } WP;
static void wip_create(void)
{
    memset(&WP, 0, sizeof(WP));
    lv_obj_t *t = page_new(P_WIP, "WI-FI IP", C_CYAN);
    kv_row(t, 42,  "SSID",    &WP.ssid);
    kv_row(t, 61,  "IP",      &WP.ip);
    kv_row(t, 80,  "MASK",    &WP.mask);
    kv_row(t, 99,  "GATEWAY", &WP.gw);
    kv_row(t, 118, "DNS",     &WP.dns);
    kv_row(t, 137, "MAC",     &WP.mac);
    kv_row(t, 156, "COUNTRY", &WP.country);
    kv_row(t, 175, "NEARBY",  &WP.aps);
}
static void wip_update(void)
{
    bool c = g.wifi_conn;
    set_txt(WP.ssid, c ? g.w_ssid : g.w_cfg_ssid[0] ? g.w_cfg_ssid : "not set");
    set_txt(WP.ip, c ? g.w_ip : "--");
    set_txt(WP.mask, c ? g.w_mask : "--");
    set_txt(WP.gw, c ? g.w_gw : "--");
    set_txt(WP.dns, c ? g.w_dns : "--");
    set_txt(WP.mac, g.w_mac);
    set_txt(WP.country, g.w_country[0] ? g.w_country : "--");
    set_fmt(WP.aps, "%d networks", g.ap_count);
}




typedef struct { lv_obj_t *stage, *down, *up, *first, *bar, *button, *btn, *net; } speed_page_t;
static speed_page_t SPD[2];

static void speed_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    speed_result_t *r = k ? &g.eth_speedtest : &g.wifi_speed;
    if (r->running) speed_cancel();
    else tool_start(k ? "espeed" : "wspeed", "");
}
static void speed_create_k(int k)
{
    speed_page_t *P = &SPD[k];
    memset(P, 0, sizeof(*P));
    uint32_t acc = k ? C_CYAN : C_PINK;
    lv_obj_t *t = page_new(k ? P_ESPEED : P_WSPEED, k ? "ETH SPEED" : "WI-FI SPEED", acc);
    P->stage = center_label(t, &lv_font_montserrat_14, C_CYAN, 42, "Ready");
    P->down = center_label(t, &lv_font_montserrat_28, C_GREEN, 62, "--");
    center_label(t, &lv_font_montserrat_12, C_DIM, 93, "DOWNLOAD Mbps");
    P->up = center_label(t, &lv_font_montserrat_20, C_CYAN, 110, "UP -- Mbps");
    P->first = center_label(t, &lv_font_montserrat_12, C_DIM, 136, "");
    P->net = center_label(t, &lv_font_montserrat_12, C_DIM, 152, "");
    P->bar = lv_bar_create(t); lv_obj_set_size(P->bar, 144, 5);
    lv_obj_align(P->bar, LV_ALIGN_TOP_MID, 0, 170);
    lv_obj_set_style_bg_color(P->bar, lv_color_hex(0x16202B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(P->bar, lv_color_hex(acc), LV_PART_INDICATOR);
    P->btn = pill_btn(t, 182, 124, k ? "RUN ETHERNET" : "RUN WI-FI", acc, speed_cb, &P->button);
    lv_obj_remove_event_cb(P->btn, speed_cb);
    lv_obj_add_event_cb(P->btn, speed_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
}
static void wspeed_create(void) { speed_create_k(0); }
static void espeed_create(void) { speed_create_k(1); }

static void speed_update_k(int k)
{
    speed_page_t *P = &SPD[k];
    speed_result_t *r = k ? &g.eth_speedtest : &g.wifi_speed;
    bool up_ok = k ? (g.eth_ip && g.eth_link) : g.wifi_conn;
    set_txt(P->stage, r->stage[0] ? r->stage : up_ok ? "Ready" : k ? "Plug in Ethernet" : "Join Wi-Fi in GUI");
    if (r->running && !r->down_ok && r->down_bytes && r->first_byte_ms) set_txt(P->down, "...");
    else if (r->down_ok) set_fmt(P->down, "%.1f", r->down_mbps); else set_txt(P->down, "--");
    if (r->up_ok) set_fmt(P->up, "UP %.1f Mbps", r->up_mbps); else set_txt(P->up, "UP -- Mbps");
    if (r->error[0]) { set_txt(P->first, r->error); set_col(P->first, C_AMBER); }
    else if (r->down_ok) { set_fmt(P->first, "First byte %d ms", r->first_byte_ms); set_col(P->first, C_DIM); }
    else { set_txt(P->first, k ? "Tester ceiling ~5-10 Mbps" : "Tester ceiling ~20-40 Mbps"); set_col(P->first, C_DIM); }
    if (k) set_fmt(P->net, g.eth_link ? "Link %dM %s" : "No link", g.eth_speed, g.eth_full ? "full" : "half");
    else if (g.wifi_conn) set_fmt(P->net, "%d dBm  CH %d  %s", g.w_rssi, g.w_chan, g.w_phy);
    else set_txt(P->net, "");
    lv_bar_set_value(P->bar, r->running ? r->progress : r->down_ok ? 100 : 0, LV_ANIM_ON);
    bool busy = !r->running && g.tool_running;
    if (busy || (!up_ok && !r->running)) lv_obj_add_state(P->btn, LV_STATE_DISABLED);
    else lv_obj_clear_state(P->btn, LV_STATE_DISABLED);
    lv_obj_set_style_opa(P->btn, lv_obj_has_state(P->btn, LV_STATE_DISABLED) ? LV_OPA_50 : LV_OPA_COVER, 0);
    set_txt(P->button, r->running ? "STOP TEST" : busy ? "TOOL BUSY" : k ? "RUN ETHERNET" : "RUN WI-FI");
}
static void wspeed_update(void) { speed_update_k(0); }
static void espeed_update(void) { speed_update_k(1); }

static void wire_create(void)
{
    lv_obj_t *t = page_new(P_WIRE, "CABLE PAIRS", C_CYAN);
    lv_obj_t *v;
    kv_row(t, 44, "LENGTH", &v);    set_txt(v, "n/a"); set_col(v, C_AMBER);
    kv_row(t, 63, "PAIR TEST", &v); set_txt(v, "n/a"); set_col(v, C_AMBER);
    kv_row(t, 82, "DATA", &v);      set_txt(v, "1-2  3-6");
    kv_row(t, 101, "SPARE", &v);    set_txt(v, "4-5  7-8");
    lv_obj_t *n = mk_label(t, &lv_font_montserrat_12, C_DIM,
        "The W5500 has no TDR or wiremap. A link only proves pairs 1-2 and 3-6. Use Cable & Link for loss, jitter and drops.");
    lv_obj_set_width(n, 168);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 126);
}

static struct { lv_obj_t *state, *src, *role, *on, *cls, *pairs, *alloc, *req, *note; } PO;
static void poe_create(void)
{
    memset(&PO, 0, sizeof(PO));
    lv_obj_t *t = page_new(P_POE, "PoE POWER", C_ORANGE);
    PO.state = center_label(t, &lv_font_montserrat_14, C_AMBER, 38, "");
    kv_row(t, 58,  "FROM",     &PO.src);
    kv_row(t, 76,  "ROLE",     &PO.role);
    kv_row(t, 94,  "ENABLED",  &PO.on);
    kv_row(t, 112, "CLASS",    &PO.cls);
    kv_row(t, 130, "PAIRS",    &PO.pairs);
    kv_row(t, 148, "ALLOCATED", &PO.alloc);
    kv_row(t, 166, "REQUESTED", &PO.req);
    PO.note = center_label(t, &lv_font_montserrat_12, C_DIM, 188, "Reported by switch");
}
static void poe_update(void)
{
    poe_info_t *p = &g.poe;
    bool fresh = poe_is_fresh();
    set_txt(PO.state, fresh ? "POWER OFFERED" : p->valid ? "OLD INFO" : "NO PoE INFO YET");
    set_col(PO.state, fresh ? C_GREEN : C_AMBER);
    if (!fresh && !p->valid) {
        set_txt(PO.src, g.eth_link ? "waiting for LLDP/CDP" : "no link");
        set_txt(PO.role, "--"); set_txt(PO.on, "--"); set_txt(PO.cls, "--");
        set_txt(PO.pairs, "--"); set_txt(PO.alloc, "--"); set_txt(PO.req, "--");
        set_txt(PO.note, "Needs LLDP or CDP");
        return;
    }
    set_fmt(PO.src, "%s  %lds ago", p->protocol, (long)((now_ms() - p->seen_ms) / 1000));
    set_txt(PO.role, p->mdi ? (p->pse ? "PSE (switch)" : "PD (device)") : "not reported");
    set_txt(PO.on, p->mdi ? (p->enabled ? "yes" : "no") : "--");
    if (p->power_class >= 0) set_fmt(PO.cls, "%d", p->power_class); else set_txt(PO.cls, "--");
    set_txt(PO.pairs, p->pairs == 1 ? "1-2 / 3-6 (A)" : p->pairs == 2 ? "4-5 / 7-8 (B)" : "--");
    if (p->allocated_mw > 0) set_fmt(PO.alloc, "%.1f W", p->allocated_mw / 1000.0f);
    else if (p->cdp_available_mw > 0) set_fmt(PO.alloc, "%.1f W avail", p->cdp_available_mw / 1000.0f);
    else if (p->med_mw > 0) set_fmt(PO.alloc, "%.1f W (MED)", p->med_mw / 1000.0f);
    else set_txt(PO.alloc, "--");
    if (p->requested_mw > 0) set_fmt(PO.req, "%.1f W", p->requested_mw / 1000.0f);
    else if (p->cdp_used_mw > 0) set_fmt(PO.req, "%.1f W used", p->cdp_used_mw / 1000.0f);
    else set_txt(PO.req, "--");
    set_txt(PO.note, "Reported by switch");
}

static lv_obj_t *display_values[4];
static void display_tap(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int f = i == 0 ? cfg_set("text_size", cfg.text_size ? "0" : "1") :
            i == 1 ? cfg_set("contrast", cfg.contrast ? "0" : "1") :
            i == 2 ? cfg_set("animations", cfg.animations ? "0" : "1") :
                     cfg_set("speed_mb", cfg.speed_mb == 5 ? "10" : cfg.speed_mb == 10 ? "20" : "5");
    cfg_apply(f);
}
static void display_create(void)
{
    static const char *names[] = { "TEXT", "CONTRAST", "ANIMATE", "DL LIMIT" };
    lv_obj_t *t = page_new(P_DISPLAY, "DISPLAY", C_VIOLET);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *row = kv_row(t, 60 + i*30, names[i], &display_values[i]);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x172D49), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_70, 0);
        lv_obj_set_style_radius(row, 7, 0);
        lv_obj_add_event_cb(row, display_tap, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    center_label(t, &lv_font_montserrat_12, C_DIM, 184, "Tap a row to change");
}
static void display_update(void)
{
    set_txt(display_values[0], cfg.text_size ? "Large" : "Standard");
    set_txt(display_values[1], cfg.contrast ? "High" : "Normal");
    set_txt(display_values[2], cfg.animations ? "On" : "Reduced");
    set_fmt(display_values[3], "%d MB", cfg.speed_mb);
}




static struct { lv_obj_t *now, *chart, *stats, *verdict, *hint; lv_chart_series_t *ser; } WG;

static void wsig_create(void)
{
    memset(&WG, 0, sizeof(WG));
    lv_obj_t *t = page_new(P_WSIG, "SIGNAL", C_GREEN);
    WG.now = center_label(t, &lv_font_montserrat_28, C_TEXT, 40, "--");
    WG.verdict = center_label(t, &lv_font_montserrat_12, C_DIM, 72, "");
    lv_obj_set_style_text_letter_space(WG.verdict, 2, 0);
    WG.chart = lv_chart_create(t);
    lv_obj_set_size(WG.chart, 180, 56);
    lv_obj_align(WG.chart, LV_ALIGN_TOP_MID, 0, 92);
    lv_chart_set_type(WG.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(WG.chart, 48);
    lv_chart_set_div_line_count(WG.chart, 3, 0);
    lv_obj_set_style_line_color(WG.chart, lv_color_hex(0x1A2A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(WG.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(WG.chart, 0, 0);
    lv_obj_set_style_pad_all(WG.chart, 0, 0);
    lv_obj_set_style_size(WG.chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(WG.chart, 2, LV_PART_ITEMS);
    lv_obj_clear_flag(WG.chart, LV_OBJ_FLAG_CLICKABLE);
    WG.ser = lv_chart_add_series(WG.chart, lv_color_hex(C_GREEN), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_range(WG.chart, LV_CHART_AXIS_PRIMARY_Y, -95, -30);
    WG.stats = center_label(t, &lv_font_montserrat_14, C_TEXT, 156, "");
    WG.hint = center_label(t, &lv_font_montserrat_12, C_DIM, 178, "-67 dBm or better for calls");
}
static void wsig_update(void)
{
    int mn = 0, mx = -127, sum = 0, n = 0;
    for (int i = 0; i < 48; i++) {
        int v = g.rssi_hist[i];
        WG.ser->y_points[i] = (v <= -100 || v == 0) ? LV_CHART_POINT_NONE : v;
        if (v > -100 && v != 0) { if (!n || v < mn) mn = v; if (v > mx) mx = v; sum += v; n++; }
    }
    lv_chart_refresh(WG.chart);
    if (!g.wifi_conn) {
        set_txt(WG.now, "--"); set_txt(WG.verdict, "NOT CONNECTED"); set_col(WG.verdict, C_DIM);
    } else {
        set_fmt(WG.now, "%d dBm", g.w_rssi);
        const char *q = g.w_rssi >= -55 ? "EXCELLENT" : g.w_rssi >= -67 ? "GOOD" : g.w_rssi >= -75 ? "FAIR" : "POOR";
        uint32_t c = g.w_rssi >= -67 ? C_GREEN : g.w_rssi >= -75 ? C_AMBER : C_RED;
        set_txt(WG.verdict, q); set_col(WG.verdict, c);
        lv_chart_set_series_color(WG.chart, WG.ser, lv_color_hex(c));
    }
    if (n) set_fmt(WG.stats, "min %d  avg %d  max %d", mn, sum / n, mx);
    else set_txt(WG.stats, "no samples yet");
}




static struct { lv_obj_t *arc, *big, *unit, *target, *lat, *jit, *cnt, *btn, *btn_lbl; } LQ;

static void lq_cb(lv_event_t *e) { (void)e; if (!g.q_running) quality_start(); }
static void qual_create(void)
{
    memset(&LQ, 0, sizeof(LQ));
    lv_obj_t *t = page_new(P_QUAL, "LINK QUALITY", C_CYAN);
    LQ.arc = lv_arc_create(t);
    lv_obj_set_size(LQ.arc, 92, 92);
    lv_obj_align(LQ.arc, LV_ALIGN_TOP_MID, 0, 38);
    lv_arc_set_rotation(LQ.arc, 135);
    lv_arc_set_bg_angles(LQ.arc, 0, 270);
    lv_arc_set_range(LQ.arc, 0, 100);
    lv_arc_set_value(LQ.arc, 0);
    lv_obj_remove_style(LQ.arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(LQ.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(LQ.arc, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(LQ.arc, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(LQ.arc, lv_color_hex(0x16202B), LV_PART_MAIN);
    lv_obj_set_style_arc_color(LQ.arc, lv_color_hex(C_CYAN), LV_PART_INDICATOR);
    LQ.big = mk_label(LQ.arc, &lv_font_montserrat_20, C_TEXT, "--");
    lv_obj_align(LQ.big, LV_ALIGN_CENTER, 0, -4);
    LQ.unit = mk_label(LQ.arc, &lv_font_montserrat_12, C_DIM, "LOSS");
    lv_obj_align(LQ.unit, LV_ALIGN_CENTER, 0, 16);
    kv_row(t, 134, "LATENCY", &LQ.lat);
    kv_row(t, 152, "JITTER",  &LQ.jit);
    LQ.target = center_label(t, &lv_font_montserrat_12, C_DIM, 170, "");
    LQ.btn = pill_btn(t, 188, 112, "RUN TEST", C_CYAN, lq_cb, &LQ.btn_lbl);
}
static void qual_update(void)
{
    int loss = g.q_sent ? (g.q_sent - g.q_recv) * 100 / g.q_sent : 0;
    if (g.q_running) {
        arc_to(LQ.arc, g.q_done);
        set_fmt(LQ.big, "%d%%", g.q_done); set_txt(LQ.unit, "TESTING");
        lv_obj_set_style_arc_color(LQ.arc, lv_color_hex(C_CYAN), LV_PART_INDICATOR);
    } else if (g.q_sent) {
        arc_to(LQ.arc, 100 - (loss > 100 ? 100 : loss));
        set_fmt(LQ.big, "%d%%", loss); set_txt(LQ.unit, "LOSS");
        lv_obj_set_style_arc_color(LQ.arc, lv_color_hex(loss > 5 ? C_RED : loss > 1 ? C_AMBER : C_GREEN), LV_PART_INDICATOR);
    } else { set_txt(LQ.big, "--"); set_txt(LQ.unit, "LOSS"); }
    if (g.q_recv) {
        set_fmt(LQ.lat, "%d / %d / %d ms", g.q_min, g.q_avg, g.q_max);
        set_fmt(LQ.jit, "%d ms", g.q_jitter);
        set_col(LQ.jit, g.q_jitter > 30 ? C_AMBER : C_TEXT);
    } else { set_txt(LQ.lat, "--"); set_txt(LQ.jit, "--"); }
    if (g.q_target[0]) set_fmt(LQ.target, "%s  %d/%d replies", g.q_target, g.q_recv, g.q_sent);
    else set_txt(LQ.target, cfg.quality_host[0] ? cfg.quality_host : "100 pings to the gateway");
    bool can = g.eth_ip || g.wifi_conn;
    if (!can && !g.q_running) lv_obj_add_state(LQ.btn, LV_STATE_DISABLED); else lv_obj_clear_state(LQ.btn, LV_STATE_DISABLED);
    lv_obj_set_style_opa(LQ.btn, lv_obj_has_state(LQ.btn, LV_STATE_DISABLED) ? LV_OPA_50 : LV_OPA_COVER, 0);
    set_txt(LQ.btn_lbl, g.q_running ? "TESTING..." : can ? "RUN TEST" : "NO NETWORK");
}





static const char *tool_tail(void)
{
    static char log[2048];
    tool_log_copy(log, sizeof(log));
    char *end = log + strlen(log);
    while (end > log && (end[-1] == '\n' || end[-1] == ' ')) *--end = 0;
    char *ln = strrchr(log, '\n');
    return ln ? ln + 1 : log;
}




static struct { lv_obj_t *target, *big, *unit, *line, *btn, *btn_lbl; } PG;
static void ping_cb(lv_event_t *e)
{
    (void)e;
    const char *t = g.eth_ip ? g.gw : (g.wifi_conn ? g.w_gw : "");
    if (t[0]) tool_start("ping", t);
}
static void ping_create(void)
{
    memset(&PG, 0, sizeof(PG));
    lv_obj_t *t = page_new(P_PING, "PING", C_GREEN);
    PG.big = center_label(t, &lv_font_montserrat_28, C_TEXT, 54, "--");
    center_label(t, &lv_font_montserrat_12, C_DIM, 88, "GATEWAY");
    PG.target = center_label(t, &lv_font_montserrat_14, C_CYAN, 108, "");
    PG.line = center_label(t, &lv_font_montserrat_12, C_DIM, 140, "5 pings to the gateway");
    PG.btn = pill_btn(t, 182, 120, "PING", C_GREEN, ping_cb, &PG.btn_lbl);
}
static void ping_update(void)
{
    const char *t = g.eth_ip ? g.gw : (g.wifi_conn ? g.w_gw : "");
    set_txt(PG.target, t[0] ? t : "no gateway");
    bool mine = !strcmp(tool_current(), "ping");
    const char *ln = tool_tail();
    if (mine) { set_txt(PG.big, "..."); set_txt(PG.line, "pinging"); set_col(PG.line, C_CYAN); }
    else if (strstr(ln, "received")) {
        set_txt(PG.line, ln); set_col(PG.line, C_DIM);
        char *avg = strstr(ln, "max ");
        int a = 0, b = 0, c = 0;
        if (avg && sscanf(avg, "max %d/%d/%d", &a, &b, &c) == 3) set_fmt(PG.big, "%d ms", b);
    }
    lv_obj_set_width(PG.line, 176); lv_label_set_long_mode(PG.line, LV_LABEL_LONG_SCROLL_CIRCULAR);
    if (!t[0] && !mine) lv_obj_add_state(PG.btn, LV_STATE_DISABLED); else lv_obj_clear_state(PG.btn, LV_STATE_DISABLED);
    lv_obj_set_style_opa(PG.btn, lv_obj_has_state(PG.btn, LV_STATE_DISABLED) ? LV_OPA_50 : LV_OPA_COVER, 0);
    set_txt(PG.btn_lbl, mine ? "PINGING..." : "PING");
}




static struct { lv_obj_t *big, *unit, *prog, *line, *btn, *btn_lbl; } HS;
static void hosts_cb(lv_event_t *e) { (void)e; if (!g.sweep_running) tool_start("sweep", ""); }
static void hosts_create(void)
{
    memset(&HS, 0, sizeof(HS));
    lv_obj_t *t = page_new(P_HOSTS, "FIND HOSTS", C_VIOLET);
    HS.big = center_label(t, &lv_font_montserrat_48, C_TEXT, 50, "--");
    center_label(t, &lv_font_montserrat_12, C_DIM, 104, "DEVICES FOUND");
    HS.line = center_label(t, &lv_font_montserrat_12, C_DIM, 128, "Pings every address on your /24");
    HS.prog = lv_bar_create(t); lv_obj_set_size(HS.prog, 144, 5);
    lv_obj_align(HS.prog, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_set_style_bg_color(HS.prog, lv_color_hex(0x16202B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(HS.prog, lv_color_hex(C_VIOLET), LV_PART_INDICATOR);
    HS.btn = pill_btn(t, 182, 120, "FIND HOSTS", C_VIOLET, hosts_cb, &HS.btn_lbl);
}
static void hosts_update(void)
{
    set_fmt(HS.big, "%d", g.sweep_found);
    const char *ln = tool_tail();
    if (g.sweep_running) { set_fmt(HS.line, "scanning .%d", g.sweep_done); set_col(HS.line, C_CYAN); }
    else { set_txt(HS.line, ln[0] && strstr(ln, "answered") ? ln : "Pings every address on your /24"); set_col(HS.line, C_DIM); }
    lv_bar_set_value(HS.prog, g.sweep_running ? g.sweep_done * 100 / 254 : 0, LV_ANIM_OFF);
    bool busy = g.tool_running && !g.sweep_running;
    if (busy) lv_obj_add_state(HS.btn, LV_STATE_DISABLED); else lv_obj_clear_state(HS.btn, LV_STATE_DISABLED);
    lv_obj_set_style_opa(HS.btn, busy ? LV_OPA_50 : LV_OPA_COVER, 0);
    set_txt(HS.btn_lbl, g.sweep_running ? "SCANNING..." : busy ? "TOOL BUSY" : "FIND HOSTS");
}




typedef struct { lv_obj_t *verdict, *bar, *line[6], *btn, *btn_lbl; const char *cmd; struct secres *res; } secpage_t;
static secpage_t SECP[12];

static void sec_cb(lv_event_t *e)
{
    secpage_t *P = lv_event_get_user_data(e);
    if (!P->res->running) tool_start(P->cmd, "");
}
static void sec_build(int idx, int page, const char *title, uint32_t acc, const char *cmd, struct secres *res, const char *run)
{
    secpage_t *P = &SECP[idx];
    memset(P, 0, sizeof(*P));
    P->cmd = cmd; P->res = res;
    lv_obj_t *t = page_new(page, title, acc);
    P->verdict = center_label(t, &lv_font_montserrat_16, C_DIM, 40, "Not run");
    lv_obj_set_style_text_letter_space(P->verdict, 1, 0);
    for (int i = 0; i < 6; i++) {
        P->line[i] = center_label(t, &lv_font_montserrat_12, C_TEXT, 64 + i * 17, "");
        lv_obj_set_width(P->line[i], band_w(64 + i * 17, 64 + i * 17 + 14, 20));
        lv_label_set_long_mode(P->line[i], LV_LABEL_LONG_DOT);
    }
    P->bar = lv_bar_create(t); lv_obj_set_size(P->bar, 130, 4);
    lv_obj_align(P->bar, LV_ALIGN_TOP_MID, 0, 170);
    lv_obj_set_style_bg_color(P->bar, lv_color_hex(0x16202B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(P->bar, lv_color_hex(acc), LV_PART_INDICATOR);
    P->btn = pill_btn(t, 184, 132, run, acc, sec_cb, &P->btn_lbl);
    lv_obj_remove_event_cb(P->btn, sec_cb);
    lv_obj_add_event_cb(P->btn, sec_cb, LV_EVENT_CLICKED, P);
}
static void sec_render(secpage_t *P, const char *run)
{
    struct secres *r = P->res;
    uint32_t vc = !r->verdict[0] ? C_DIM : r->level == 2 ? C_RED : r->level == 1 ? C_AMBER : C_GREEN;
    set_txt(P->verdict, r->running ? "Testing..." : r->verdict[0] ? r->verdict : "Not run");
    set_col(P->verdict, r->running ? C_CYAN : vc);
    for (int i = 0; i < 6; i++) set_txt(P->line[i], i < r->n ? r->line[i] : "");
    lv_bar_set_value(P->bar, r->running ? r->done : r->verdict[0] ? 100 : 0, LV_ANIM_OFF);
    bool busy = g.tool_running && !r->running;
    if (busy) lv_obj_add_state(P->btn, LV_STATE_DISABLED); else lv_obj_clear_state(P->btn, LV_STATE_DISABLED);
    lv_obj_set_style_opa(P->btn, busy ? LV_OPA_50 : LV_OPA_COVER, 0);
    set_txt(P->btn_lbl, r->running ? "TESTING..." : busy ? "TOOL BUSY" : run);
}
#define SECPG(ix, page, title, acc, cmd, res, run, fn)                       \
    static void fn##_create(void) { sec_build(ix, page, title, acc, cmd, res, run); } \
    static void fn##_update(void) { sec_render(&SECP[ix], run); }
SECPG(0, P_SECROUTER, "ROUTER SCAN", C_ORANGE, "secrouter", &g.sec_router, "SCAN", secrouter)
SECPG(1, P_SECWIFI,   "WI-FI AUDIT", C_GREEN,  "secwifi",   &g.sec_wifi,   "AUDIT", secwifi)
SECPG(2, P_SECROGUE,  "ROGUE AP",    C_AMBER,  "secrogue",  &g.sec_rogue,  "SCAN", secrogue)
SECPG(3, P_SECDNS,    "DNS CHECK",   C_BLUE,   "secdns",    &g.sec_dns,    "CHECK", secdns)
SECPG(4, P_SECSNMP,   "SNMP CHECK",  C_VIOLET, "secsnmp",   &g.sec_snmp,   "CHECK", secsnmp)
SECPG(5, P_SECUPNP,   "UPnP SCAN",   C_CYAN,   "secupnp",   &g.sec_upnp,   "SCAN", secupnp)
SECPG(6, P_SECTLS,    "TLS CERT",    C_GOLD,   "sectls",    &g.sec_tls,    "CHECK", sectls)
SECPG(7, P_SECDHCP,   "DHCP WATCH",  C_BLUE,   "secdhcp",   &g.sec_dhcp,   "CHECK", secdhcp)
SECPG(8, P_SECSEG,    "SEGMENT",     C_ORANGE, "secseg",    &g.sec_seg,    "PROBE", secseg)
SECPG(9, P_SECARP,    "ARP TEST",    C_PINK,   "secarp",    &g.sec_arp,    "RUN TEST", secarp)
SECPG(10, P_BUFFER,   "BUFFERBLOAT", C_CYAN,   "buffer",    &g.sec_buffer, "RUN", buffer)




static lv_obj_t *tap_row(lv_obj_t *t, int y, const char *name, lv_event_cb_t cb, int idx)
{
    int w = band_w(y, y + 19, 40); if (w > 170) w = 170;
    lv_obj_t *row = lv_obj_create(t);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, w, 19);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *k = mk_label(row, &lv_font_montserrat_12, C_DIM, name);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *b = lv_btn_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 70, 18);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x16222E), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x24405A), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x2A3E54), 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    else lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *v = mk_label(b, &lv_font_montserrat_12, C_TEXT, "");
    lv_obj_center(v);
    return v;
}




enum { B_ON, B_PIN, B_PHONES, B_ROWS };
static struct { lv_obj_t *val[B_ROWS], *name, *state, *forget_lbl; uint32_t forget_t; } BT;

static void bt_tap(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == B_ON) cfg_apply(cfg_set("ble", cfg.ble ? "0" : "1"));
    else if (i == B_PIN) cfg_apply(cfg_set("ble_pin", cfg.ble_pin ? "0" : "1"));
}
static void bt_forget(lv_event_t *e)
{
    (void)e;

    uint32_t t = lv_tick_get();
    if (BT.forget_t && t - BT.forget_t < 3000) { BT.forget_t = 0; ble_forget_all(); toast_show(LV_SYMBOL_TRASH "  Phones forgotten", C_AMBER); }
    else BT.forget_t = t ? t : 1;
}
static void btset_create(void)
{
    memset(&BT, 0, sizeof(BT));
    lv_obj_t *t = page_new(P_BTSET, "BLUETOOTH", C_VIOLET);
    BT.state = center_label(t, &lv_font_montserrat_14, C_VIOLET, 42, "");
    BT.val[B_ON]     = tap_row(t, 66,  "BLUETOOTH", bt_tap, B_ON);
    BT.val[B_PIN]    = tap_row(t, 90,  "PIN PAIRING", bt_tap, B_PIN);
    BT.val[B_PHONES] = tap_row(t, 114, "PAIRED", NULL, B_PHONES);
    BT.name = center_label(t, &lv_font_montserrat_12, C_DIM, 140, "");
    pill_btn(t, 166, 160, "FORGET PHONES", C_AMBER, bt_forget, &BT.forget_lbl);
}
static void btset_update(void)
{
    set_txt(BT.val[B_ON], cfg.ble ? "on" : "off");
    set_col(BT.val[B_ON], cfg.ble ? C_GREEN : C_AMBER);
    set_txt(BT.val[B_PIN], cfg.ble_pin ? "required" : "off");
    set_col(BT.val[B_PIN], cfg.ble_pin ? C_GREEN : C_AMBER);
    set_fmt(BT.val[B_PHONES], "%d phone%s", g.ble_bonds, g.ble_bonds == 1 ? "" : "s");
    set_txt(BT.state, !cfg.ble ? "OFF" : g.ble_secure ? "PAIRED + SECURE" : g.ble_conn ? "PHONE CONNECTED" : g.ble_on ? "READY TO PAIR" : "STARTING");
    set_col(BT.state, !cfg.ble ? C_DIM : g.ble_conn ? C_GREEN : C_VIOLET);
    set_txt(BT.name, g.ble_name);
    if (BT.forget_t && lv_tick_get() - BT.forget_t >= 3000) BT.forget_t = 0;
    set_txt(BT.forget_lbl, BT.forget_t ? "TAP AGAIN" : "FORGET PHONES");
}




static struct { lv_obj_t *qr, *frame, *hint, *name, *sub, *none; char last[100]; } BC;

static void btconn_create(void)
{
    memset(&BC, 0, sizeof(BC));
    lv_obj_t *t = page_new(P_BTCONN, "BLUETOOTH", C_VIOLET);
    BC.hint = center_label(t, &lv_font_montserrat_12, C_DIM, 40, "SCAN, THEN TAP CONNECT");
    lv_obj_set_style_text_letter_space(BC.hint, 1, 0);
    BC.frame = lv_obj_create(t);
    lv_obj_remove_style_all(BC.frame);
    lv_obj_set_size(BC.frame, 116, 116);
    lv_obj_align(BC.frame, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(BC.frame, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(BC.frame, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(BC.frame, 8, 0);
    lv_obj_clear_flag(BC.frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    BC.qr = lv_qrcode_create(BC.frame, 104, lv_color_hex(0x000000), lv_color_hex(0xFFFFFF));
    lv_obj_center(BC.qr);
    BC.none = mk_label(t, &lv_font_montserrat_14, C_TEXT,
                       "Put web/ble.html on an HTTPS site (GitHub Pages), then enter its link in phone GUI > Settings > Bluetooth GUI link.");
    lv_obj_set_width(BC.none, 168);
    lv_label_set_long_mode(BC.none, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(BC.none, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(BC.none, LV_ALIGN_TOP_MID, 0, 64);
    BC.name = center_label(t, &lv_font_montserrat_12, C_TEXT, 182, "");
    BC.sub = center_label(t, &lv_font_montserrat_12, C_DIM, 198, "");
}
static void btconn_update(void)
{
    bool have = cfg.ble_url[0] != 0;
    if (have) {
        lv_obj_clear_flag(BC.frame, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(BC.none, LV_OBJ_FLAG_HIDDEN);
        if (strcmp(cfg.ble_url, BC.last)) {
            snprintf(BC.last, sizeof(BC.last), "%s", cfg.ble_url);
            lv_qrcode_update(BC.qr, cfg.ble_url, strlen(cfg.ble_url));
        }
    } else { lv_obj_add_flag(BC.frame, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(BC.none, LV_OBJ_FLAG_HIDDEN); }
    set_txt(BC.name, g.ble_name);
    if (!cfg.ble) set_txt(BC.sub, "Bluetooth is off (Settings)");
    else if (g.ble_secure) set_txt(BC.sub, "Paired - GUI live over Bluetooth");
    else if (g.ble_conn) set_txt(BC.sub, cfg.ble_pin ? "Enter the PIN shown here" : "Phone connected");
    else set_txt(BC.sub, cfg.ble_pin ? "PIN pairing on" : "No PIN needed");
    set_col(BC.sub, g.ble_secure ? C_GREEN : C_DIM);
    set_txt(BC.hint, g.ble_conn ? "PHONE CONNECTED" : have ? "SCAN, THEN TAP CONNECT" : "ONE-TIME SETUP");
    set_col(BC.hint, g.ble_conn ? C_VIOLET : C_DIM);
}




static lv_obj_t *pin_box, *pin_num;
static void pin_overlay_update(void)
{
    if (g.ble_pairing && !pin_box) {
        pin_box = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(pin_box);
        lv_obj_set_size(pin_box, SCR, SCR);
        lv_obj_set_style_radius(pin_box, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(pin_box, lv_color_hex(0x0B0E1A), 0);
        lv_obj_set_style_bg_opa(pin_box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pin_box, 3, 0);
        lv_obj_set_style_border_color(pin_box, lv_color_hex(C_VIOLET), 0);
        lv_obj_add_flag(pin_box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(pin_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *ic = mk_label(pin_box, &lv_font_montserrat_28, C_VIOLET, LV_SYMBOL_BLUETOOTH);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 34);
        lv_obj_t *h = mk_label(pin_box, &lv_font_montserrat_14, C_TEXT, "PAIRING PIN");
        lv_obj_set_style_text_letter_space(h, 2, 0);
        lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 76);
        pin_num = mk_label(pin_box, &lv_font_montserrat_48, C_CYAN, "");
        lv_obj_align(pin_num, LV_ALIGN_CENTER, 0, 10);
        lv_obj_t *s = mk_label(pin_box, &lv_font_montserrat_12, C_DIM, "Type this on your phone");
        lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 172);
        lv_obj_set_style_opa(pin_box, 0, 0);
        anim(pin_box, a_opa, 0, 255, 250, 0, lv_anim_path_linear);
        lv_disp_trig_activity(NULL);
    }
    if (pin_box) {
        if (!g.ble_pairing) { lv_obj_del(pin_box); pin_box = pin_num = NULL; return; }
        set_fmt(pin_num, "%06u", (unsigned)g.ble_passkey);
    }
}





static void (*const builders[P_COUNT])(void) = {
    [P_HOME] = home_create, [P_DEV] = dev_create, [P_POWER] = power_create, [P_ALERT] = alert_create,
    [P_SWITCH] = switch_create, [P_SWDET] = swdet_create, [P_POE] = poe_create, [P_CABLE] = cable_create,
    [P_PORT] = port_create, [P_PKT] = pkt_create, [P_IP] = ip_create, [P_WIRE] = wire_create,
    [P_WIFI] = wifi_create, [P_WSIG] = wsig_create, [P_WINFO] = winfo_create, [P_WIP] = wip_create, [P_SCAN] = scan_create,
    [P_TEST] = test_create, [P_WSPEED] = wspeed_create, [P_ESPEED] = espeed_create, [P_QUAL] = qual_create,
    [P_PING] = ping_create, [P_HOSTS] = hosts_create, [P_BUFFER] = buffer_create,
    [P_SECROUTER] = secrouter_create, [P_SECWIFI] = secwifi_create, [P_SECROGUE] = secrogue_create,
    [P_SECDNS] = secdns_create, [P_SECSNMP] = secsnmp_create, [P_SECUPNP] = secupnp_create,
    [P_SECTLS] = sectls_create, [P_SECDHCP] = secdhcp_create, [P_SECSEG] = secseg_create, [P_SECARP] = secarp_create,
    [P_SET] = set_create, [P_DISPLAY] = display_create, [P_BTSET] = btset_create,
    [P_CONN] = conn_create, [P_BTCONN] = btconn_create,
};
static void (*const updaters[P_COUNT])(void) = {
    [P_HOME] = home_update, [P_DEV] = dev_update, [P_POWER] = power_update, [P_ALERT] = alert_update,
    [P_SWITCH] = switch_update, [P_SWDET] = swdet_update, [P_POE] = poe_update, [P_CABLE] = cable_update,
    [P_PORT] = port_update, [P_PKT] = pkt_update, [P_IP] = ip_update, [P_WIRE] = NULL,
    [P_WIFI] = wifi_update, [P_WSIG] = wsig_update, [P_WINFO] = winfo_update, [P_WIP] = wip_update, [P_SCAN] = scan_update,
    [P_TEST] = test_update, [P_WSPEED] = wspeed_update, [P_ESPEED] = espeed_update, [P_QUAL] = qual_update,
    [P_PING] = ping_update, [P_HOSTS] = hosts_update, [P_BUFFER] = buffer_update,
    [P_SECROUTER] = secrouter_update, [P_SECWIFI] = secwifi_update, [P_SECROGUE] = secrogue_update,
    [P_SECDNS] = secdns_update, [P_SECSNMP] = secsnmp_update, [P_SECUPNP] = secupnp_update,
    [P_SECTLS] = sectls_update, [P_SECDHCP] = secdhcp_update, [P_SECSEG] = secseg_update, [P_SECARP] = secarp_update,
    [P_SET] = set_update, [P_DISPLAY] = display_update, [P_BTSET] = btset_update,
    [P_CONN] = conn_update, [P_BTCONN] = btconn_update,
};
static bool built[P_COUNT];

static void page_build(int p)
{
    if (p < 0 || p >= P_COUNT || built[p]) return;
    builders[p]();
    built[p] = true;
}


static bool page_near(int c, int p)
{
    if (p == c) return true;
    int cc = page_col(c), pc = page_col(p), cr = page_row(c), pr = page_row(p);
    if (cc == pc) return abs(cr - pr) == 1;
    return cr == 0 && pr == 0 && abs(cc - pc) == 1;
}

static void pages_ensure(int c)
{
    for (int p = 0; p < P_COUNT; p++) {
        if (!page_near(c, p) && built[p]) {
            lv_obj_clean(tiles[p]);
            built[p] = false;
            rings[p] = NULL;
        }
    }
    for (int p = 0; p < P_COUNT; p++) if (page_near(c, p)) page_build(p);
}

static void page_activated(int i)
{
    cur_page = i;
    pages_ensure(i);
    ui_update();
    dots_set(i);
    page_enter(i);
}

static void tv_event_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *act = lv_tileview_get_tile_act(tv);
    for (int i = 0; i < P_COUNT; i++)
        if (tiles[i] == act && i != cur_page) { page_activated(i); break; }
}

static void goto_async(void *p) { ui_goto((int)(intptr_t)p, false); }


static void gesture_cb(lv_event_t *e)
{
    (void)e;
    if (page_row(cur_page) == 0) return;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
    int c = page_col(cur_page);
    if (d == LV_DIR_LEFT && c < COL_COUNT - 1) c++;
    else if (d == LV_DIR_RIGHT && c > 0) c--;
    else return;
    lv_indev_wait_release(lv_indev_get_act());
    lv_async_call(goto_async, (void *)(intptr_t)col_first[c]);
}

void ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    tv = lv_tileview_create(scr);
    lv_obj_set_style_bg_color(tv, lv_color_hex(C_BG), 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_anim_time(tv, 180, 0);

    for (int i = 0; i < P_COUNT; i++) {
        int c = page_col(i), r = page_row(i), n = col_rows(c);
        lv_dir_t dir = 0;
        if (r == 0) {
            if (c > 0) dir |= LV_DIR_LEFT;
            if (c < COL_COUNT - 1) dir |= LV_DIR_RIGHT;
        } else dir |= LV_DIR_TOP;
        if (r < n - 1) dir |= LV_DIR_BOTTOM;
        tiles[i] = lv_tileview_add_tile(tv, c, r, dir);
        lv_obj_set_style_bg_color(tiles[i], lv_color_hex(C_BG), 0);
        lv_obj_set_style_bg_opa(tiles[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(tiles[i], LV_OBJ_FLAG_SCROLL_ELASTIC);
    }
    pages_ensure(0);

    lv_obj_add_event_cb(tv, tv_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    dots_create();
    dots_set(0);
    toast_create();
    splash_create();
    ui_update();
}

void ui_goto(int page, bool animate)
{
    if (page < 0 || page >= P_COUNT) return;
    page_build(page);
    lv_obj_set_tile_id(tv, page_col(page), page_row(page), animate ? LV_ANIM_ON : LV_ANIM_OFF);
    if (!animate) page_activated(page);
}

int ui_page_count(void) { return P_COUNT; }

void ui_update(void)
{
    int appearance = cfg.text_size | (cfg.contrast << 1) | (cfg.animations << 2);
    if (appearance != s_display_cfg) {

        s_display_cfg = appearance;
        for (int p = 0; p < P_COUNT; p++) if (built[p]) {
            lv_obj_clean(tiles[p]); built[p] = false; rings[p] = NULL;
        }
        pages_ensure(cur_page);
        lv_obj_set_style_anim_time(tv, cfg.animations ? 180 : 0, 0);
    }


    uint32_t ev = ui_take_events();
    if (ev) lv_disp_trig_activity(NULL);
    if (ev & EV_BLE_PAIRED) toast_show(LV_SYMBOL_BLUETOOTH "  Phone paired", C_GREEN);
    else if (ev & EV_BLE_PAIRFAIL) toast_show(LV_SYMBOL_CLOSE "  Pairing failed", C_RED);
    else if (ev & EV_BLE_CONN)   toast_show(LV_SYMBOL_BLUETOOTH "  Phone connected", C_VIOLET);
    else if (ev & EV_PHONE_AP) toast_show(LV_SYMBOL_WIFI "  Phone joined", C_VIOLET);
    else if (ev & EV_SWITCH) toast_show(LV_SYMBOL_OK "  Switch found", C_ORANGE);
    else if (ev & EV_TEST_DONE)
        toast_show(g.test_result == 1 ? LV_SYMBOL_OK "  Test passed" :
                   g.test_result == 2 ? LV_SYMBOL_WARNING "  Test: warnings" : LV_SYMBOL_CLOSE "  Test failed",
                   g.test_result == 1 ? C_GREEN : g.test_result == 2 ? C_AMBER : C_RED);
    else if (ev & EV_LINK_UP)   toast_show(LV_SYMBOL_UP "  Link up", C_GREEN);
    else if (ev & EV_LINK_DOWN) toast_show(LV_SYMBOL_DOWN "  Link down", C_RED);
    else if (ev & EV_ALERT) {
        bst_lock();
        char m[64] = "Network alert";
        for (int i = 0; i < g.alert_n; i++) if (g.alerts[i].lvl >= 1) { snprintf(m, sizeof(m), "%s", g.alerts[i].txt); break; }
        bst_unlock();
        char s[72]; snprintf(s, sizeof(s), LV_SYMBOL_WARNING "  %.30s", m);
        toast_show(s, C_AMBER);
    }
    else if (ev & EV_BATT_LOW)  toast_show(LV_SYMBOL_BATTERY_1 "  Battery low", C_RED);
    else if (ev & EV_QUALITY_DONE) {
        char s[48]; int loss = g.q_sent ? (g.q_sent - g.q_recv) * 100 / g.q_sent : 100;
        snprintf(s, sizeof(s), "Link: %d%% loss, %d ms", loss, g.q_avg);
        toast_show(s, loss > 1 ? C_AMBER : C_GREEN);
    }
    else if (ev & EV_BLE_DISC)  toast_show(LV_SYMBOL_BLUETOOTH "  Phone left", C_DIM);

    pin_overlay_update();


    for (int p = 0; p < P_COUNT; p++) if (built[p] && updaters[p]) updaters[p]();
}

int ui_current_page(void) { return cur_page; }
