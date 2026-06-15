// 自动生成 — 显示素材位图 (4bpp 灰度)
#pragma once
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    uint16_t w;
    uint16_t h;
} asset_bitmap_t;

extern const asset_bitmap_t bmp_label_co2;
extern const asset_bitmap_t bmp_label_temp;
extern const asset_bitmap_t bmp_label_humi;
extern const asset_bitmap_t bmp_label_ppm;
extern const asset_bitmap_t bmp_label_degc;
extern const asset_bitmap_t bmp_label_pct;
extern const asset_bitmap_t bmp_label_good;
extern const asset_bitmap_t bmp_label_warn;
extern const asset_bitmap_t bmp_label_bad;
extern const asset_bitmap_t bmp_digit_0;
extern const asset_bitmap_t bmp_digit_1;
extern const asset_bitmap_t bmp_digit_2;
extern const asset_bitmap_t bmp_digit_3;
extern const asset_bitmap_t bmp_digit_4;
extern const asset_bitmap_t bmp_digit_5;
extern const asset_bitmap_t bmp_digit_6;
extern const asset_bitmap_t bmp_digit_7;
extern const asset_bitmap_t bmp_digit_8;
extern const asset_bitmap_t bmp_digit_9;
extern const asset_bitmap_t bmp_digit_dot;

// 便捷宏
#define BMP(name) (&bmp_##name)
