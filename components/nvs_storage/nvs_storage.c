/*
 * nvs_storage.c — 轻量 NVS 键值存储实现
 */
#include "nvs_storage.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "nvs";

#define NVS_NS  "dryrack"

static esp_err_t open_ns(nvs_open_mode_t mode, nvs_handle_t *out)
{
    return nvs_open(NVS_NS, mode, out);
}

esp_err_t nvs_storage_init(void)
{
    /* NVS 分区由 main 中的 nvs_flash_init() 负责初始化，这里只做可用性检查 */
    nvs_handle_t handle;
    esp_err_t ret = open_ns(NVS_READONLY, &handle);
    if (ret == ESP_OK) {
        nvs_close(handle);
        ESP_LOGI(TAG, "NVS 存储就绪");
        return ESP_OK;
    }

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        /* 命名空间还没创建过，属于正常情况（首次启动） */
        ESP_LOGI(TAG, "NVS 命名空间尚未建立（首次启动）");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "NVS 不可用: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t nvs_load_str(const char *key, char *out, size_t out_size)
{
    if (key == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = open_ns(NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t len = out_size;
    ret = nvs_get_str(handle, key, out, &len);
    nvs_close(handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = '\0';
    }
    return ret;
}

esp_err_t nvs_save_str(const char *key, const char *value)
{
    if (key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = open_ns(NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    if (value[0] == '\0') {
        ret = nvs_erase_key(handle, key);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
    } else {
        ret = nvs_set_str(handle, key, value);
    }

    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "保存 %s 失败: %s", key, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t nvs_load_u8(const char *key, uint8_t *out, uint8_t def_value)
{
    if (key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = open_ns(NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        *out = def_value;
        return ret;
    }

    uint8_t val = def_value;
    ret = nvs_get_u8(handle, key, &val);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        /* 首次使用：把默认值写进去，后续读取就不用再兜底 */
        nvs_set_u8(handle, key, def_value);
        nvs_commit(handle);
        *out = def_value;
        ret = ESP_OK;
    } else if (ret == ESP_OK) {
        *out = val;
    } else {
        *out = def_value;
    }

    nvs_close(handle);
    return ret;
}

esp_err_t nvs_save_u8(const char *key, uint8_t value)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = open_ns(NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(handle, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

esp_err_t nvs_erase(const char *key)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = open_ns(NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_erase_key(handle, key);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}
