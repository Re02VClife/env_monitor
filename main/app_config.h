/*
 * app_config.h — 环境监测仪集中配置
 * 所有引脚定义、I2C 地址、阈值参数在此集中管理
 */
#pragma once

#include "hal/gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * I2C 总线引脚 — 两条独立总线，物理隔离传感器
 * ================================================================ */

/* I2C 总线 0：AHT30 (0x38) */
#define I2C0_SCL_IO     GPIO_NUM_5
#define I2C0_SDA_IO     GPIO_NUM_6

/* I2C 总线 1：SGP30 (0x58) */
#define I2C1_SCL_IO     GPIO_NUM_19
#define I2C1_SDA_IO     GPIO_NUM_20

/* ================================================================
 * SGP30 基线策略 — 智能保存，保证长期稳定准确
 *
 * 启动时恢复上次保存的基线（维持连续性）
 * 仅在 CO2 连续低于阈值达到指定时长后才保存新基线
 * （确保只记录「真正的新鲜空气」）
 * ================================================================ */

#define BASELINE_CLEAN_CO2_MAX      450     /* 视为"新鲜空气"的 CO2 上限 (ppm) */
#define BASELINE_CLEAN_HOLD_S       3600    /* 连续低 CO2 保持多久才保存基线 (秒) */

/* ================================================================
 * 采样与阈值参数
 * ================================================================ */

#define SAMPLE_INTERVAL_MS          1000    /* 传感器采样间隔 */
#define CO2_WARN_THRESHOLD          800     /* CO2 黄色警告 (ppm) */
#define CO2_ALERT_THRESHOLD         1200    /* CO2 红色警报 (ppm) */

/* ================================================================
 * NVS 存储命名空间
 * ================================================================ */

#define NVS_NAMESPACE           "env_monitor"
#define NVS_KEY_CO2_BASELINE    "co2_base"
#define NVS_KEY_TVOC_BASELINE   "tvoc_base"

#ifdef __cplusplus
}
#endif
