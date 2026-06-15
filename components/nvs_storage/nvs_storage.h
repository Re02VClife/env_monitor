/*
 * nvs_storage.h — NVS 非易失存储封装
 * 用于 SGP30 基线的断电持久化
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 NVS 存储分区
 * @return ESP_OK 成功
 */
esp_err_t nvs_storage_init(void);

/**
 * @brief 保存 SGP30 IAQ 基线
 */
esp_err_t nvs_save_sgp30_baseline(uint16_t co2_baseline, uint16_t tvoc_baseline);

/**
 * @brief 读取上次保存的 SGP30 IAQ 基线
 * @return ESP_OK 成功，ESP_ERR_NOT_FOUND 无保存记录
 */
esp_err_t nvs_load_sgp30_baseline(uint16_t *co2_baseline, uint16_t *tvoc_baseline);

#ifdef __cplusplus
}
#endif
