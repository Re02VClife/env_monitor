/*
 * app_display.c — 干湿提醒界面（直接帧缓冲绘制）
 *
 * 横屏 320×240 黑底布局:
 * ┌────────────────────────────────────┐ y=0
 * │  干湿提醒                 已联网   │ 标题 + 联网状态
 * ├────────────────────────────────────┤ y=38
 * │   温度            湿度             │ 标签
 * │  25.5 °C          65 %             │ 数值
 * ├────────────────────────────────────┤ y=122
 * │ ████████████░░░░     适宜          │ 湿度条 + 干湿状态
 * ├────────────────────────────────────┤ y=168
 * │ 露点 12.3°C                        │
 * │ 绝对湿度 9.8 g/m3                  │
 * └────────────────────────────────────┘ y=240
 */
#include "app_display.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "display_assets.h"
#include "esp_log.h"
#include "st7789.h"

static const char *TAG = "display";

/* ---- RGB565 颜色 ---- */
#define RGB565(r, g, b)  (uint16_t)(((r) & 0x1F) << 11 | ((g) & 0x3F) << 5 | ((b) & 0x1F))

#define COLOR_BG        0x0000
#define COLOR_WHITE     RGB565(31, 63, 31)
#define COLOR_GRAY      RGB565( 8, 16,  8)
#define COLOR_DIM       RGB565(14, 28, 14)
#define COLOR_GREEN     RGB565( 0, 60, 10)
#define COLOR_CYAN      RGB565( 0, 55, 55)
#define COLOR_YELLOW    RGB565(31, 48,  0)
#define COLOR_RED       RGB565(31,  4,  0)
#define COLOR_BAR_BG    RGB565( 4,  8,  4)

/* ---- 版式常量 ---- */
#define LCD_W        320
#define LCD_H        240
#define HALF_W       160
#define TITLE_BOTTOM 38
#define CONTENT_TOP  42   /* 标题栏以下的内容区起点：数字与标签都在这里，必须整块擦 */
#define STATE_LINE   172

#define BIG_DIGIT_H  34   /* digit 素材行高 */
#define SMALL_DIGIT_H 18  /* sdigit 素材行高 */

static uint16_t *fb = NULL;

/* ---- 数字集：0-9 / '.' / '-' ---- */
static const asset_bitmap_t *digits[12];
static const asset_bitmap_t *sdigits[12];

static display_net_t s_net = DISPLAY_NET_OFFLINE;

/* ================================================================
 * 绘图原语
 * ================================================================ */

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (fb == NULL) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return;

    for (int row = 0; row < h; row++) {
        uint16_t *dst = fb + (y + row) * LCD_W + x;
        for (int col = 0; col < w; col++) dst[col] = color;
    }
}

/* 4bpp 位图 alpha 混合绘制 */
static void draw_bmp(int x, int y, const asset_bitmap_t *bmp, uint16_t fg)
{
    if (fb == NULL || bmp == NULL || bmp->data == NULL) return;
    if (x + bmp->w <= 0 || y + bmp->h <= 0 || x >= LCD_W || y >= LCD_H) return;

    int w = bmp->w, h = bmp->h;
    int row_bytes = (w + 1) / 2;

    for (int row = 0; row < h; row++) {
        int sy = y + row;
        if (sy < 0 || sy >= LCD_H) continue;
        uint16_t *dst = fb + sy * LCD_W + x;

        for (int col = 0; col < w; col++) {
            int sx = x + col;
            if (sx < 0 || sx >= LCD_W) continue;

            uint8_t byte = bmp->data[row * row_bytes + col / 2];
            int a = (col & 1) ? (byte & 0x0F) : (byte >> 4);
            if (a == 0) continue;
            if (a >= 15) { dst[col] = fg; continue; }

            uint16_t bg = dst[col];
            int r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * (15 - a)) / 15;
            int g = (((fg >>  5) & 0x3F) * a + ((bg >>  5) & 0x3F) * (15 - a)) / 15;
            int b = (( fg        & 0x1F) * a + ( bg        & 0x1F) * (15 - a)) / 15;
            dst[col] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

static const asset_bitmap_t *pick_glyph(char c, const asset_bitmap_t **set)
{
    if (c >= '0' && c <= '9') return set[c - '0'];
    if (c == '.') return set[10];
    if (c == '-') return set[11];
    return NULL;
}

/* 绘制数字串，返回像素宽度
 *
 * 字符默认顶部对齐，但两种符号要特殊处理：
 *   - 小数点素材只有 6px（大号）/ 3px（小号）高，必须落到基线，否则会飘在数字顶端
 *   - 负号较矮，垂直居中才不会变成下标
 */
static int draw_string(int x, int y, const char *s, uint16_t color,
                       const asset_bitmap_t **set)
{
    const int base_h = set[0]->h;   /* 以数字 0 的高度为基准 */
    int start = x;

    while (*s) {
        const asset_bitmap_t *bmp = pick_glyph(*s, set);
        if (bmp) {
            int dy = 0;
            if (*s == '.') {
                dy = base_h - bmp->h;         /* 小数点底部对齐 */
            } else if (*s == '-') {
                dy = (base_h - bmp->h) / 2;   /* 负号垂直居中 */
            }
            draw_bmp(x, y + dy, bmp, color);
            x += bmp->w + 1;
        }
        s++;
    }
    return (x - start) > 0 ? (x - start - 1) : 0;
}

static int string_width(const char *s, const asset_bitmap_t **set)
{
    int w = 0;
    while (*s) {
        const asset_bitmap_t *bmp = pick_glyph(*s, set);
        if (bmp) w += bmp->w + 1;
        s++;
    }
    return w > 0 ? w - 1 : 0;
}

/* 数字 + 单位，水平居中在 cx，返回总宽 */
static int draw_number_unit(int cx, int y, const char *num,
                            const asset_bitmap_t *unit,
                            uint16_t color,
                            const asset_bitmap_t **set, int digit_h)
{
    int num_w = string_width(num, set);
    int unit_w = (unit != NULL) ? unit->w : 0;
    int gap = (unit != NULL) ? 6 : 0;
    int total = num_w + gap + unit_w;

    int x = cx - total / 2;
    draw_string(x, y, num, color, set);

    if (unit != NULL) {
        int unit_y = y + digit_h - unit->h;
        if (unit_y < y) unit_y = y;
        draw_bmp(x + num_w + gap, unit_y, unit, color);
    }
    return total;
}

/* ================================================================
 * 颜色 / 文案映射
 * ================================================================ */

static uint16_t level_color(dry_level_t level)
{
    switch (level) {
    case DRY_LEVEL_DRY:    return COLOR_CYAN;
    case DRY_LEVEL_NORMAL: return COLOR_GREEN;
    case DRY_LEVEL_DAMP:   return COLOR_YELLOW;
    case DRY_LEVEL_WET:    return COLOR_RED;
    default:               return COLOR_WHITE;
    }
}

static const asset_bitmap_t *level_bmp(dry_level_t level)
{
    switch (level) {
    case DRY_LEVEL_DRY:    return BMP(status_dry);
    case DRY_LEVEL_NORMAL: return BMP(status_normal);
    case DRY_LEVEL_DAMP:   return BMP(status_damp);
    case DRY_LEVEL_WET:    return BMP(status_wet);
    default:               return BMP(hint_none);
    }
}

/* ================================================================
 * 静态元素
 * ================================================================ */

static void draw_title_bar(void)
{
    fill_rect(0, 0, LCD_W, TITLE_BOTTOM, COLOR_BG);

    const asset_bitmap_t *title = BMP(title);
    draw_bmp((LCD_W - title->w) / 2, 5, title, COLOR_WHITE);

    fill_rect(0, TITLE_BOTTOM, LCD_W, 1, COLOR_GRAY);
}

/* 温度/湿度标签、分栏竖线、底部两行标签
 * 这些元素都落在内容区里，每次整块重绘后必须补画 */
static void draw_labels(void)
{
    draw_bmp(HALF_W / 2 - BMP(label_temp)->w / 2, 44, BMP(label_temp), COLOR_DIM);
    draw_bmp(HALF_W + HALF_W / 2 - BMP(label_humi)->w / 2, 44, BMP(label_humi), COLOR_DIM);

    /* 左右分栏竖线 */
    fill_rect(HALF_W, 46, 1, 68, COLOR_GRAY);

    /* 底部两行标签 */
    draw_bmp(12, 176, BMP(label_dew), COLOR_DIM);
    draw_bmp(12, 204, BMP(label_abs), COLOR_DIM);
}

/* 预计干衣时间：一行「预计 X.X 小时」/「预计 NN 分钟」 */
static void draw_eta(const env_reading_t *r)
{
    const int y_base = 169;   /* 这一行按底部对齐 */

    if (r == NULL || !r->valid || r->eta_min < 0) {
        draw_bmp(16, y_base - BMP(hint_none)->h, BMP(hint_none), COLOR_DIM);
        return;
    }

    char num[12];
    const asset_bitmap_t *unit;

    if (r->eta_min < 60) {
        snprintf(num, sizeof(num), "%d", r->eta_min);
        unit = BMP(unit_min);
    } else {
        float hours = (float)r->eta_min / 60.0f;
        snprintf(num, sizeof(num), (hours < 10.0f) ? "%.1f" : "%.0f", (double)hours);
        unit = BMP(unit_hour);
    }

    int x = 16;
    draw_bmp(x, y_base - BMP(label_eta)->h, BMP(label_eta), COLOR_DIM);
    x += BMP(label_eta)->w + 6;

    int w = draw_string(x, y_base - SMALL_DIGIT_H, num, COLOR_WHITE, sdigits);
    x += w + 6;

    draw_bmp(x, y_base - unit->h, unit, COLOR_DIM);
}

/* ================================================================
 * 公开接口
 * ================================================================ */

void app_display_init(void)
{
    fb = st7789_get_fb();
    if (fb == NULL) {
        ESP_LOGE(TAG, "帧缓冲不可用，界面将不可用");
        return;
    }

    digits[0]  = BMP(digit_0);  digits[1]  = BMP(digit_1);
    digits[2]  = BMP(digit_2);  digits[3]  = BMP(digit_3);
    digits[4]  = BMP(digit_4);  digits[5]  = BMP(digit_5);
    digits[6]  = BMP(digit_6);  digits[7]  = BMP(digit_7);
    digits[8]  = BMP(digit_8);  digits[9]  = BMP(digit_9);
    digits[10] = BMP(digit_dot);
    digits[11] = BMP(digit_minus);

    sdigits[0]  = BMP(sdigit_0);  sdigits[1]  = BMP(sdigit_1);
    sdigits[2]  = BMP(sdigit_2);  sdigits[3]  = BMP(sdigit_3);
    sdigits[4]  = BMP(sdigit_4);  sdigits[5]  = BMP(sdigit_5);
    sdigits[6]  = BMP(sdigit_6);  sdigits[7]  = BMP(sdigit_7);
    sdigits[8]  = BMP(sdigit_8);  sdigits[9]  = BMP(sdigit_9);
    sdigits[10] = BMP(sdigit_dot);
    sdigits[11] = BMP(sdigit_minus);

    fill_rect(0, 0, LCD_W, LCD_H, COLOR_BG);
    draw_title_bar();
    draw_labels();
    app_display_set_net(DISPLAY_NET_OFFLINE);

    st7789_flush();
    ESP_LOGI(TAG, "界面初始化完成 (320x240 横屏)");
}

void app_display_update(const env_reading_t *reading)
{
    if (fb == NULL || reading == NULL) return;

    char buf[16];

    /* 从标题栏下方整块重绘：
     * 温度/湿度数字（y=72）和底部信息都在这个区间里，不一起擦就会叠字 */
    fill_rect(0, CONTENT_TOP, LCD_W, LCD_H - CONTENT_TOP, COLOR_BG);

    /* 标签与竖线刚被擦掉，补回来 */
    draw_labels();

    /* ---------- 1. 温度（左半） ---------- */
    if (reading->valid) {
        snprintf(buf, sizeof(buf), "%.1f", (double)reading->temperature);
        draw_number_unit(HALF_W / 2, 72, buf, BMP(label_degc),
                         COLOR_WHITE, digits, BIG_DIGIT_H);
    } else {
        draw_bmp(HALF_W / 2 - BMP(hint_none)->w / 2, 80, BMP(hint_none), COLOR_DIM);
    }

    /* ---------- 2. 湿度（右半） ---------- */
    if (reading->valid) {
        snprintf(buf, sizeof(buf), "%.0f", (double)reading->humidity);
        draw_number_unit(HALF_W + HALF_W / 2, 72, buf, BMP(label_pct),
                         COLOR_WHITE, digits, BIG_DIGIT_H);
    } else {
        draw_bmp(HALF_W + HALF_W / 2 - BMP(hint_none)->w / 2, 80,
                 BMP(hint_none), COLOR_DIM);
    }

    /* ---------- 3. 状态区 ---------- */
    uint16_t color = reading->valid ? level_color(reading->level) : COLOR_DIM;

    /* 湿度进度条 0~100% → 0~150px */
    const int bar_x = 16, bar_y = 124, bar_w = 150, bar_h = 16;
    fill_rect(bar_x, bar_y, bar_w, bar_h, COLOR_BAR_BG);

    int fill_w = 0;
    if (reading->valid) {
        float rh = reading->humidity;
        if (rh < 0.0f) rh = 0.0f;
        if (rh > 100.0f) rh = 100.0f;
        fill_w = (int)(rh * (float)bar_w / 100.0f);
    }
    if (fill_w > 0) {
        fill_rect(bar_x, bar_y, fill_w, bar_h, color);
    }

    /* 干湿状态大字 */
    if (reading->valid) {
        const asset_bitmap_t *st = level_bmp(reading->level);
        draw_bmp(200, 114, st, color);
    }

    /* 预计干衣时间（进度条下方） */
    draw_eta(reading);

    /* 分隔线 */
    fill_rect(0, STATE_LINE, LCD_W, 1, COLOR_GRAY);

    /* ---------- 4. 底部：露点 / 绝对湿度 ---------- */
    if (reading->valid) {
        snprintf(buf, sizeof(buf), "%.1f", (double)reading->dew_point);
        int w = draw_string(62, 178, buf, COLOR_WHITE, sdigits);
        draw_bmp(62 + w + 6, 178 + SMALL_DIGIT_H - BMP(label_degc)->h,
                 BMP(label_degc), COLOR_DIM);

        snprintf(buf, sizeof(buf), "%.1f", (double)reading->abs_humidity);
        w = draw_string(108, 206, buf, COLOR_WHITE, sdigits);
        draw_bmp(108 + w + 6, 206 + SMALL_DIGIT_H - BMP(unit_gm3)->h,
                 BMP(unit_gm3), COLOR_DIM);
    } else {
        draw_bmp(62, 178, BMP(hint_none), COLOR_DIM);
        draw_bmp(108, 206, BMP(hint_none), COLOR_DIM);
    }

    /* ---------- 5. 低温提示 ---------- */
    if (reading->valid && reading->cold) {
        draw_bmp(200, 200, BMP(hint_cold), COLOR_YELLOW);
    }

    st7789_flush();
}

void app_display_set_net(display_net_t state)
{
    if (fb == NULL) return;

    s_net = state;

    const asset_bitmap_t *bmp;
    uint16_t color;

    switch (state) {
    case DISPLAY_NET_ONLINE:
        bmp = BMP(net_online);
        color = COLOR_GREEN;
        break;
    case DISPLAY_NET_PROV:
        bmp = BMP(net_prov);
        color = COLOR_YELLOW;
        break;
    case DISPLAY_NET_OFFLINE:
    default:
        bmp = BMP(net_offline);
        color = COLOR_RED;
        break;
    }

    int x = LCD_W - 10 - bmp->w;
    fill_rect(230, 4, LCD_W - 230, TITLE_BOTTOM - 4, COLOR_BG);
    draw_bmp(x, 10, bmp, color);

    st7789_flush();
}

void app_display_off(void)
{
    if (fb == NULL) return;
    st7789_display_off();
}

void app_display_on(void)
{
    if (fb == NULL) return;
    st7789_display_on();
    draw_title_bar();
    draw_labels();
    app_display_set_net(s_net);
    st7789_flush();
}
