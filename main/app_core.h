/*
 * app_core.h — 传感器采集核心逻辑
 * 负责: I2C 总线初始化、传感器状态机、数据采集与推送
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化传感器子系统
 *        - 创建两条 I2C 总线
 *        - 初始化 AHT30、SGP30、NVS 存储、WS2812B
 * @return ESP_OK 成功
 */
esp_err_t app_core_init(void);

/**
 * @brief 传感器采集任务函数（作为 FreeRTOS 任务运行）
 *        状态机: INIT → WARMUP(15s) → RUNNING
 *        每秒采集一次传感器数据并推送到显示层
 */
void app_core_sensor_task(void *arg);

#ifdef __cplusplus
}
#endif
