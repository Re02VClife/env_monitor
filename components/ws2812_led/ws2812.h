/*
 * ws2812.h — WS2812B 可寻址 RGB LED 驱动（原生 RMT 实现，无外部依赖）
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 预设颜色 */
typedef enum {
    LED_COLOR_OFF = 0,
    LED_COLOR_GREEN,    /* 干燥 / 适宜，可以晾 */
    LED_COLOR_YELLOW,   /* 偏潮 */
    LED_COLOR_RED,      /* 潮湿 */
    LED_COLOR_BLUE,     /* 未联网 / 配网中 */
} led_color_t;

/**
 * @brief 初始化 WS2812B（RMT TX 通道 + copy 编码器）
 */
esp_err_t ws2812_init(void);

/**
 * @brief 直接设置 RGB 颜色（0-255）
 */
esp_err_t ws2812_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief 设置预设颜色
 */
esp_err_t ws2812_set_color(led_color_t color);

#ifdef __cplusplus
}
#endif
