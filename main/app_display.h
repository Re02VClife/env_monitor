/*
 * app_display.h — 干湿提醒界面（直接帧缓冲绘制，无 LVGL）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 右上角联网状态 */
typedef enum {
    DISPLAY_NET_OFFLINE = 0,   /* 未联网 */
    DISPLAY_NET_PROV,          /* 配网模式 */
    DISPLAY_NET_ONLINE,        /* 已联网 */
} display_net_t;

/**
 * @brief 初始化界面（清屏并绘制静态元素）
 */
void app_display_init(void);

/**
 * @brief 刷新温湿度与干湿状态
 */
void app_display_update(const env_reading_t *reading);

/**
 * @brief 更新右上角联网状态（只重绘这一小块）
 */
void app_display_set_net(display_net_t state);

/**
 * @brief 关闭屏幕（黑屏 + DISPOFF + 关背光）
 */
void app_display_off(void);

/**
 * @brief 重新点亮屏幕并全量重绘
 */
void app_display_on(void);

#ifdef __cplusplus
}
#endif
