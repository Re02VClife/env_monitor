/*
 * sgp30.h — SGP30 气体传感器 I2C 驱动
 * 支持 CO2eq (ppm) 和 TVOC (ppb) 测量
 * 具备 IAQ 基线校准与湿度补偿功能
 */
#pragma once

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SGP30 测量结果 */
typedef struct {
    uint16_t co2_ppm;   /* CO2 等效浓度 (ppm) */
    uint16_t tvoc_ppb;  /* TVOC 浓度 (ppb) */
} sgp30_data_t;

/* IAQ 基线值（用于加速启动校准） */
typedef struct {
    uint16_t co2_baseline;
    uint16_t tvoc_baseline;
} sgp30_baseline_t;

/**
 * @brief 初始化 SGP30 传感器（读取 Feature Set 版本）
 * @param bus_handle I2C 总线句柄
 * @return ESP_OK 成功
 */
esp_err_t sgp30_init(i2c_master_bus_handle_t bus_handle);

/**
 * @brief 硬件复位 — I2C General Call Reset
 *        将 SGP30 恢复到出厂状态，比 IAQ_INIT 更彻底
 *        复位后必须重新调用 sgp30_iaq_init()
 * @param bus_handle I2C 总线句柄
 * @return ESP_OK 成功
 */
esp_err_t sgp30_reset(i2c_master_bus_handle_t bus_handle);

/**
 * @brief 启动 IAQ 初始化（15 秒预热）—— 上电后必须调用
 *        内部会阻塞等待传感器完成初始化
 * @param bus_handle I2C 总线句柄
 * @return ESP_OK 成功
 */
esp_err_t sgp30_iaq_init(i2c_master_bus_handle_t bus_handle);

/**
 * @brief 执行一次 IAQ 测量
 * @param bus_handle I2C 总线句柄
 * @param data      输出 CO2 和 TVOC 值
 * @return ESP_OK 成功
 */
esp_err_t sgp30_read(i2c_master_bus_handle_t bus_handle, sgp30_data_t *data);

/**
 * @brief 读取当前 IAQ 基线值（用于 NVS 持久化）
 */
esp_err_t sgp30_get_baseline(i2c_master_bus_handle_t bus_handle, sgp30_baseline_t *b);

/**
 * @brief 写入 IAQ 基线值（从 NVS 恢复）
 */
esp_err_t sgp30_set_baseline(i2c_master_bus_handle_t bus_handle, const sgp30_baseline_t *b);

/**
 * @brief 设置绝对湿度补偿值（提高测量精度）
 * @param abs_humidity_gm3 绝对湿度 (g/m³)
 */
esp_err_t sgp30_set_humidity(i2c_master_bus_handle_t bus_handle, float abs_humidity_gm3);

#ifdef __cplusplus
}
#endif
