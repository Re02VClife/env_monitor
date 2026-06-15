/*
 * app_display.h — 简易仪表盘界面（直接帧缓冲绘制，无 LVGL）
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化仪表盘（清屏并绘制静态元素）
 */
void app_display_init(void);

/**
 * @brief 更新仪表盘数据
 * @param temperature 温度 (°C)
 * @param humidity    相对湿度 (%RH)
 * @param co2_ppm     CO2 浓度 (ppm)
 * @param tvoc_ppb    TVOC 浓度 (ppb)
 */
void app_display_update(float temperature, float humidity,
                        uint16_t co2_ppm, uint16_t tvoc_ppb);

#ifdef __cplusplus
}
#endif
