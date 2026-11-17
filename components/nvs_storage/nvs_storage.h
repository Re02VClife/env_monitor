/*
 * nvs_storage.h — 轻量 NVS 键值存储
 *
 * 用于保存 WiFi 凭据、webhook 地址、上报开关等运行时配置。
 * 不做初始化检查的调用方需保证 main 中已 nvs_flash_init()。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 检查 NVS 可用（只打日志，不重复初始化）
 */
esp_err_t nvs_storage_init(void);

/**
 * @brief 读取字符串；键不存在或长度不符返回 ESP_ERR_NVS_NOT_FOUND
 */
esp_err_t nvs_load_str(const char *key, char *out, size_t out_size);

/**
 * @brief 写入字符串（空字符串等同于删除该键）
 */
esp_err_t nvs_save_str(const char *key, const char *value);

/**
 * @brief 读取 u8，键不存在时写入默认值并返回默认值
 */
esp_err_t nvs_load_u8(const char *key, uint8_t *out, uint8_t def_value);

/**
 * @brief 写入 u8
 */
esp_err_t nvs_save_u8(const char *key, uint8_t value);

/**
 * @brief 删除某个键
 */
esp_err_t nvs_erase(const char *key);

#ifdef __cplusplus
}
#endif
