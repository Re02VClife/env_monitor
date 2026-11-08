/*
 * app_power.h — 低功耗模式与唤醒管理
 *
 * 低功耗设计：
 *   - 长按开关机键（或网页 /api/sleep）进入深睡
 *   - 深睡期间关闭屏幕显示与背光、关闭 WiFi 与指示灯
 *   - 每 SLEEP_INTERVAL_MIN 分钟由 RTC 定时器自动唤醒，采集一次并上报后继续睡
 *   - 按 BOOT 键（GPIO0，RTC IO）立即唤醒并回到正常显示模式
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_sleep.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 本次启动的运行模式 */
typedef enum {
    BOOT_MODE_NORMAL = 0,      /* 冷启动 / 复位 / BOOT 键唤醒 → 亮屏正常运行 */
    BOOT_MODE_TIMER_CYCLE,     /* 定时唤醒 → 静默采集一次，上报后继续睡 */
} boot_mode_t;

/**
 * @brief 根据唤醒原因判断本次启动模式
 */
boot_mode_t app_power_detect_boot_mode(void);

/**
 * @brief 唤醒原因的中文描述（用于开机日志）
 */
const char *app_power_wakeup_cause_text(void);

/**
 * @brief 进入低功耗：关屏、关外设、配置唤醒源，然后深睡（不返回）
 */
void app_power_enter_sleep(void);

/**
 * @brief 主动关闭屏幕（不进入深睡，正常模式下也可用）
 */
void app_power_screen_off(void);

/**
 * @brief 重新点亮屏幕
 */
void app_power_screen_on(void);

#ifdef __cplusplus
}
#endif
