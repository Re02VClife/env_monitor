// 自动生成 — 显示素材位图 (4bpp 灰度)
// 由 generate_display_assets.py 生成，请勿手工修改
#pragma once
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    uint16_t w;
    uint16_t h;
} asset_bitmap_t;

extern const asset_bitmap_t bmp_title;
extern const asset_bitmap_t bmp_label_temp;
extern const asset_bitmap_t bmp_label_humi;
extern const asset_bitmap_t bmp_label_degc;
extern const asset_bitmap_t bmp_label_pct;
extern const asset_bitmap_t bmp_label_dew;
extern const asset_bitmap_t bmp_label_abs;
extern const asset_bitmap_t bmp_unit_gm3;
extern const asset_bitmap_t bmp_status_dry;
extern const asset_bitmap_t bmp_status_normal;
extern const asset_bitmap_t bmp_status_damp;
extern const asset_bitmap_t bmp_status_wet;
extern const asset_bitmap_t bmp_net_online;
extern const asset_bitmap_t bmp_net_offline;
extern const asset_bitmap_t bmp_net_prov;
extern const asset_bitmap_t bmp_hint_cold;
extern const asset_bitmap_t bmp_hint_none;
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
extern const asset_bitmap_t bmp_digit_minus;
extern const asset_bitmap_t bmp_sdigit_0;
extern const asset_bitmap_t bmp_sdigit_1;
extern const asset_bitmap_t bmp_sdigit_2;
extern const asset_bitmap_t bmp_sdigit_3;
extern const asset_bitmap_t bmp_sdigit_4;
extern const asset_bitmap_t bmp_sdigit_5;
extern const asset_bitmap_t bmp_sdigit_6;
extern const asset_bitmap_t bmp_sdigit_7;
extern const asset_bitmap_t bmp_sdigit_8;
extern const asset_bitmap_t bmp_sdigit_9;
extern const asset_bitmap_t bmp_sdigit_dot;
extern const asset_bitmap_t bmp_sdigit_minus;

// 便捷宏
#define BMP(name) (&bmp_##name)
