/*
 * app_display.c — 简易仪表盘（直接帧缓冲绘制）
 *
 * 横屏 320×240 黑底白字布局:
 * ┌────────────────────────────────────┐
 * │  CO2    888    ppm                │  CO2 大数字
 * ├────────────────────────────────────┤
 * │    温度       │      湿度          │
 * │   25.5 °C    │      65 %          │  温湿度
 * ├────────────────────────────────────┤
 * │ ████████░░░░░  良好               │  CO2 进度条
 * └────────────────────────────────────┘
 */
#include "app_display.h"
#include "app_config.h"
#include "st7789.h"
#include "display_assets.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "display";

/* ---- RGB565 颜色 ---- */
#define RGB565(r,g,b)  (uint16_t)(((r)&0x1F)<<11 | ((g)&0x3F)<<5 | ((b)&0x1F))

#define COLOR_BG        0x0000              /* 纯黑 */
#define COLOR_WHITE     RGB565(31,63,31)    /* 白色 */
#define COLOR_GRAY      RGB565(10,20,10)    /* 暗灰 (分隔线等) */
#define COLOR_GREEN     RGB565(0, 55, 0)    /* CO2 良好 */
#define COLOR_YELLOW    RGB565(31,60, 0)    /* CO2 一般 */
#define COLOR_RED       RGB565(30, 0, 0)    /* CO2 差 */
#define COLOR_BAR_BG    RGB565(4,  8, 4)    /* 进度条背景 */

/* ---- 常量 ---- */
#define LCD_W  320
#define LCD_H  240
#define HALF_W 160   /* 左右半栏宽度 */

static uint16_t *fb = NULL;

/* ================================================================
 * 绘图原语
 * ================================================================ */

/* 填充矩形 */
static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
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

/* 绘制 4bpp 位图 (alpha 混合到背景) */
static void draw_bmp(int x, int y, const asset_bitmap_t *bmp, uint16_t fg)
{
    if (bmp == NULL || bmp->data == NULL) return;
    int w = bmp->w, h = bmp->h;
    if (x + w <= 0 || y + h <= 0 || x >= LCD_W || y >= LCD_H) return;

    int row_bytes = (w + 1) / 2;
    for (int row = 0; row < h; row++) {
        int sy = y + row;
        if (sy < 0 || sy >= LCD_H) continue;
        uint16_t *dst = fb + sy * LCD_W + x;
        for (int col = 0; col < w; col++) {
            int sx = x + col;
            if (sx < 0 || sx >= LCD_W) continue;
            int a = (col & 1) ? (bmp->data[row * row_bytes + col/2] & 0x0F)
                              : (bmp->data[row * row_bytes + col/2] >> 4);
            if (a == 0) continue;
            if (a >= 15) { dst[col] = fg; continue; }

            uint16_t bg = dst[col];
            int r = (((fg>>11)&0x1F) * a + ((bg>>11)&0x1F) * (15-a)) / 15;
            int g = (((fg>>5)&0x3F)  * a + ((bg>>5)&0x3F)  * (15-a)) / 15;
            int b = (( fg    &0x1F) * a + ( bg    &0x1F) * (15-a)) / 15;
            dst[col] = (uint16_t)((r<<11)|(g<<5)|b);
        }
    }
}

/* 以指定位图集绘制数字串，返回像素宽度 */
static int draw_digits(int x, int y, const char *s, uint16_t color,
                       const asset_bitmap_t **set)
{
    int start_x = x;
    while (*s) {
        const asset_bitmap_t *bmp = NULL;
        if (*s >= '0' && *s <= '9') bmp = set[*s - '0'];
        else if (*s == '.')         bmp = set[10];
        if (bmp) { draw_bmp(x, y, bmp, color); x += bmp->w + 1; }
        s++;
    }
    return x - start_x - 1; /* 去掉最后 1px 间距 */
}

/* 数字 + 单位组合绘制（不重叠） */
static void draw_value_unit(int x, int y, const char *val,
                            const asset_bitmap_t *unit_bmp,
                            uint16_t color, const asset_bitmap_t **digits)
{
    int vw = draw_digits(x, y, val, color, digits);
    draw_bmp(x + vw + 6, y, unit_bmp, color); /* 6px 间隙 */
}

/* ================================================================
 * 数字集
 * ================================================================ */
static const asset_bitmap_t *digits[11]; /* 0-9 + dot */

static void init_digits(void)
{
    digits[0]=BMP(digit_0); digits[1]=BMP(digit_1); digits[2]=BMP(digit_2);
    digits[3]=BMP(digit_3); digits[4]=BMP(digit_4); digits[5]=BMP(digit_5);
    digits[6]=BMP(digit_6); digits[7]=BMP(digit_7); digits[8]=BMP(digit_8);
    digits[9]=BMP(digit_9); digits[10]=BMP(digit_dot);
}

/* ================================================================
 * 居中绘制
 * ================================================================ */
static void draw_centered(int cx, int y, const asset_bitmap_t *bmp, uint16_t c)
{
    if (bmp) draw_bmp(cx - bmp->w / 2, y, bmp, c);
}

static void draw_digits_centered(int cx, int y, const char *s,
                                  uint16_t c, const asset_bitmap_t **set)
{
    int tw = 0;
    for (const char *p = s; *p; p++) {
        if (*p >= '0' && *p <= '9') tw += set[*p-'0']->w + 1;
        else if (*p == '.')         tw += set[10]->w + 1;
    }
    if (tw > 0) tw -= 1;
    draw_digits(cx - tw/2, y, s, c, set);
}

/* ================================================================
 * CO2 辅助
 * ================================================================ */
static uint16_t co2_color(uint16_t co2)
{
    if (co2 < CO2_WARN_THRESHOLD)  return COLOR_GREEN;
    if (co2 < CO2_ALERT_THRESHOLD) return COLOR_YELLOW;
    return COLOR_RED;
}

static const asset_bitmap_t *co2_label_bmp(uint16_t co2)
{
    if (co2 < CO2_WARN_THRESHOLD)  return BMP(label_good);
    if (co2 < CO2_ALERT_THRESHOLD) return BMP(label_warn);
    return BMP(label_bad);
}

/* ================================================================
 * 公开接口
 * ================================================================ */

void app_display_init(void)
{
    fb = st7789_get_fb();
    if (fb == NULL) { ESP_LOGE(TAG, "帧缓冲不可用!"); return; }
    init_digits();

    fill_rect(0, 0, LCD_W, LCD_H, COLOR_BG);
    st7789_flush();
    ESP_LOGI(TAG, "仪表盘初始化完成 (黑底白字 横屏320x240)");
}

void app_display_update(float temperature, float humidity,
                        uint16_t co2_ppm, uint16_t tvoc_ppb)
{
    if (fb == NULL) return;

    uint16_t c2c = co2_color(co2_ppm); /* CO2 颜色 */
    char buf[16];

    /* ---- 上半区清除 ---- */
    fill_rect(0, 0, LCD_W, 160, COLOR_BG);

    /* ============================================================
     * 1. CO2 区域 (y: 5 ~ 55)
     * ============================================================ */
    draw_bmp(12, 5, BMP(label_co2), COLOR_WHITE);

    /* CO2 数值居中 */
    snprintf(buf, sizeof(buf), "%d", (int)co2_ppm);
    draw_digits_centered(LCD_W / 2, 5, buf, c2c, digits);

    /* ppm 标签 — 跟在数字后面 */
    {
        /* 计算数字总宽度来找 ppm 位置 */
        int tw = 0;
        for (const char *p = buf; *p; p++) {
            if (*p >= '0' && *p <= '9') tw += digits[*p-'0']->w + 1;
        }
        tw -= 1;
        int num_start = LCD_W / 2 - tw / 2;
        draw_bmp(num_start + tw + 8, 15, BMP(label_ppm), COLOR_GRAY);
    }

    /* ---- 分隔线 ---- */
    fill_rect(0, 60, LCD_W, 1, COLOR_GRAY);

    /* ============================================================
     * 2. 温度 + 湿度 (左右半栏, y: 65 ~ 145)
     *    标签在上，数值+单位在下，彻底避免重叠
     * ============================================================ */

    /* ---- 左栏: 温度 ---- */
    draw_centered(HALF_W / 2, 72, BMP(label_temp), COLOR_WHITE);

    snprintf(buf, sizeof(buf), "%.1f", (double)temperature);
    {
        int tw = 0;
        for (const char *p = buf; *p; p++) {
            if (*p >= '0' && *p <= '9') tw += digits[*p-'0']->w + 1;
            else if (*p == '.')         tw += digits[10]->w + 1;
        }
        tw -= 1;
        int unit_w = BMP(label_degc)->w;
        int total_w = tw + 6 + unit_w; /* 数字 + 6px间距 + 单位 */
        int num_x = (HALF_W - total_w) / 2;
        /* 数值垂直: 标签(26px高)在72, 数值在72+26+8=106 */
        draw_value_unit(num_x, 106, buf, BMP(label_degc), COLOR_WHITE, digits);
    }

    /* ---- 右栏: 湿度 ---- */
    draw_centered(HALF_W + HALF_W / 2, 72, BMP(label_humi), COLOR_WHITE);

    snprintf(buf, sizeof(buf), "%.0f", (double)humidity);
    {
        int tw = 0;
        for (const char *p = buf; *p; p++) {
            if (*p >= '0' && *p <= '9') tw += digits[*p-'0']->w + 1;
        }
        tw -= 1;
        int unit_w = BMP(label_pct)->w;
        int total_w = tw + 6 + unit_w;
        int num_x = HALF_W + (HALF_W - total_w) / 2;
        draw_value_unit(num_x, 106, buf, BMP(label_pct), COLOR_WHITE, digits);
    }

    /* ---- 中部分隔线 ---- */
    fill_rect(HALF_W, 75, 1, 60, COLOR_GRAY);

    /* ---- 下部分隔线 ---- */
    fill_rect(0, 152, LCD_W, 1, COLOR_GRAY);

    /* ============================================================
     * 3. CO2 进度条 (y: 160 ~ 185)
     * ============================================================ */
    fill_rect(0, 155, LCD_W, 45, COLOR_BG);

    /* 条背景 */
    fill_rect(10, 160, LCD_W - 20, 16, COLOR_BAR_BG);

    /* 条前景 (400-2000 → 0..300) */
    int bw = (int)((int32_t)(co2_ppm - 400) * (LCD_W - 20) / 1600);
    if (bw < 0) bw = 0;
    if (bw > LCD_W - 20) bw = LCD_W - 20;
    fill_rect(10, 160, bw, 16, c2c);

    /* 状态标签 */
    draw_bmp(10, 180, co2_label_bmp(co2_ppm), c2c);

    /* ---- 底部分隔线 ---- */
    fill_rect(0, 200, LCD_W, 1, COLOR_GRAY);

    /* ============================================================
     * 4. TVOC 信息 (y: 205 ~ 235)
     * ============================================================ */
    fill_rect(0, 203, LCD_W, 37, COLOR_BG);
    snprintf(buf, sizeof(buf), "TVOC: %d ppb", (int)tvoc_ppb);
    /* 简单文本: 用数字位图绘制 TVOC 数值 */
    /* "TVOC:" 无位图，先跳过；仅用数字绘制 ppb 值作为信息展示 */
    /* TODO: 后续可添加 ASCII 字体 */

    /* ---- 刷新 ---- */
    st7789_flush();
}
