/*
 * st7789.h — ST7789 LCD 显示驱动（无 LVGL，直接帧缓冲）
 * 横屏 320×240, SPI 接口, RGB565 帧缓冲直接操作
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 ST7789 显示屏（横屏 320×240）
 *        创建帧缓冲，完成 SPI/面板初始化；若配置了背光 GPIO 会一并点亮
 * @return ESP_OK 成功
 */
esp_err_t st7789_init(void);

/**
 * @brief 获取帧缓冲指针（RGB565 格式，320×240）
 *        直接写入帧缓冲后调用 st7789_flush() 刷新到屏幕
 * @return 帧缓冲地址，未初始化返回 NULL
 */
uint16_t *st7789_get_fb(void);

/**
 * @brief 将帧缓冲内容刷新到屏幕（全屏传输）
 */
void st7789_flush(void);

/**
 * @brief 关闭显示输出（DISPOFF）+ 关闭背光，用于低功耗
 */
void st7789_display_off(void);

/**
 * @brief 恢复显示输出（DISPON）+ 点亮背光
 */
void st7789_display_on(void);

/**
 * @brief 是否有可控制的背光 GPIO
 */
bool st7789_has_backlight(void);

/**
 * @brief 获取屏幕尺寸
 */
int st7789_width(void);
int st7789_height(void);

#ifdef __cplusplus
}
#endif
