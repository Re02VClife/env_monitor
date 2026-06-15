/*
 * ws2812.h — WS2812B 可寻址 RGB LED 驱动
 * 使用 RMT 外设，通过 led_strip 组件封装
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CO2 浓度对应的 LED 颜色 */
typedef enum {
    LED_COLOR_GREEN,   /* < 800ppm, 空气质量良好 */
    LED_COLOR_YELLOW,  /* 800-1200ppm, 空气质量一般 */
    LED_COLOR_RED,     /* > 1200ppm, 空气质量差 */
    LED_COLOR_BLUE,    /* 启动/初始化状态 */
    LED_COLOR_OFF,     /* 关闭 */
} led_color_t;

/**
 * @brief 初始化 WS2812B LED（RMT 后端）
 * @return ESP_OK 成功
 */
esp_err_t ws2812_init(void);

/**
 * @brief 直接设置 RGB 颜色（0-255）
 */
esp_err_t ws2812_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief 根据 CO2 浓度自动设置 LED 颜色
 * @param co2_ppm CO2 浓度值
 */
esp_err_t ws2812_set_co2_level(uint16_t co2_ppm);

/**
 * @brief 设置预设颜色
 */
esp_err_t ws2812_set_color(led_color_t color);

#ifdef __cplusplus
}
#endif
