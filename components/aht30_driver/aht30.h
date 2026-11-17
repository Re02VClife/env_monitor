/*
 * aht30.h — AHT30 温湿度传感器 I2C 驱动
 * 协议兼容 AHT20，I2C 地址 0x38
 */
#pragma once

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AHT30 测量结果 */
typedef struct {
    float temperature;  /* 摄氏度 (°C) */
    float humidity;     /* 相对湿度 (%RH) */
} aht30_data_t;

/**
 * @brief 初始化 AHT30 传感器（发送校准命令）
 * @param bus_handle I2C 总线句柄
 * @return ESP_OK 成功，否则失败
 */
esp_err_t aht30_init(i2c_master_bus_handle_t bus_handle);

/**
 * @brief 触发测量并读取温湿度数据
 * @param bus_handle I2C 总线句柄
 * @param data      输出测量结果
 * @return ESP_OK 成功，否则失败
 */
esp_err_t aht30_read(i2c_master_bus_handle_t bus_handle, aht30_data_t *data);

/**
 * @brief 软复位 AHT30（不常用）
 * @param bus_handle I2C 总线句柄
 * @return ESP_OK 成功，否则失败
 */
esp_err_t aht30_soft_reset(i2c_master_bus_handle_t bus_handle);

#ifdef __cplusplus
}
#endif
