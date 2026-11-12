/*
 * app_core.h — 温湿度采集与干湿判定
 *
 * 硬件上 SGP30 已拆除，本模块只驱动 AHT30；
 * 并在相对湿度基础上派生出绝对湿度、露点与「晾衣干湿等级」。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 晾衣场景的干湿等级，按湿度从干到湿排列 */
typedef enum {
    DRY_LEVEL_DRY = 0,   /* 干燥：衣物易干 */
    DRY_LEVEL_NORMAL,    /* 适宜 */
    DRY_LEVEL_DAMP,      /* 偏潮：干得慢 */
    DRY_LEVEL_WET,       /* 潮湿：不易干、易返潮 */
    DRY_LEVEL_COUNT,
} dry_level_t;

/* 一次完整的环境读数 */
typedef struct {
    float temperature;    /* 温度 (°C) */
    float humidity;       /* 相对湿度 (%RH) */
    float abs_humidity;   /* 绝对湿度 (g/m³) */
    float dew_point;      /* 露点温度 (°C) */
    dry_level_t level;    /* 干湿等级 */
    int   eta_min;        /* 预计干衣剩余时间（分钟），<0 表示暂时无法估算 */
    bool  eta_by_trend;   /* true=按湿度衰减趋势外推；false=按 VPD 物理模型 */
    bool  cold;           /* 低温环境（蒸发慢） */
    bool  valid;          /* 数据是否有效 */
} env_reading_t;

/**
 * @brief 初始化传感器子系统（I2C + AHT30 + WS2812B）
 *        AHT30 不存在时不会返回错误，只是后续读数失败。
 */
esp_err_t app_core_init(void);

/**
 * @brief 同步读取一次环境数据（内部含 ~80ms 测量等待）
 */
esp_err_t app_core_read(env_reading_t *out);

/**
 * @brief 连续采样多次取平均，用于低功耗唤醒时的一次性采集
 * @param samples 采样次数（建议 3）
 * @param gap_ms  每次之间的间隔
 */
esp_err_t app_core_read_average(env_reading_t *out, int samples, int gap_ms);

/**
 * @brief 取最近一次有效读数（供 HTTP 接口 / 上报使用）
 * @return true 表示有有效数据
 */
bool app_core_get_last(env_reading_t *out);

/**
 * @brief 传感器采集任务（每秒刷新一次界面与指示灯）
 */
void app_core_sensor_task(void *arg);

/**
 * @brief 干湿等级的中文名（"干燥" / "适宜" / "偏潮" / "潮湿"）
 */
const char *dry_level_text(dry_level_t level);

/**
 * @brief 干湿等级的英文短码（dry / normal / damp / wet），用于 JSON
 */
const char *dry_level_code(dry_level_t level);

/**
 * @brief 重置干衣时间估算的历史数据
 *
 * 挂上新衣服（湿度开始上升）时调用，避免旧曲线干扰预测。
 */
void app_core_eta_reset(void);

/**
 * @brief 按干湿等级设置 WS2812B 颜色
 */
esp_err_t app_core_led_set_level(dry_level_t level);

/**
 * @brief 冻结采集任务对屏幕的刷新（进入低功耗关屏前调用）
 *
 * 否则关屏后采集任务仍会写帧缓冲并 flush，把屏幕重新点亮。
 */
void app_core_suspend_updates(void);

#ifdef __cplusplus
}
#endif
