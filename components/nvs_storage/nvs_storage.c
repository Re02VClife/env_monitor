/*
 * nvs_storage.c — NVS 存储实现
 */
#include "nvs_storage.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "nvs";

#define NVS_NAMESPACE           "env_monitor"
#define NVS_KEY_CO2_BASELINE    "co2_base"
#define NVS_KEY_TVOC_BASELINE   "tvoc_base"

esp_err_t nvs_storage_init(void)
{
    /* NVS 在 main.c 中已由 nvs_flash_init() 初始化，此处仅验证 */
    ESP_LOGI(TAG, "NVS 存储就绪");
    return ESP_OK;
}

esp_err_t nvs_save_sgp30_baseline(uint16_t co2_baseline, uint16_t tvoc_baseline)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_u16(handle, NVS_KEY_CO2_BASELINE, co2_baseline);
    if (ret == ESP_OK) {
        ret = nvs_set_u16(handle, NVS_KEY_TVOC_BASELINE, tvoc_baseline);
    }

    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "基线已保存: CO2=0x%04X, TVOC=0x%04X", co2_baseline, tvoc_baseline);
        }
    }

    nvs_close(handle);
    return ret;
}

esp_err_t nvs_load_sgp30_baseline(uint16_t *co2_baseline, uint16_t *tvoc_baseline)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "打开 NVS 失败（可能尚无数据）: %s", esp_err_to_name(ret));
        return ret;
    }

    uint16_t co2 = 0, tvoc = 0;
    ret = nvs_get_u16(handle, NVS_KEY_CO2_BASELINE, &co2);
    if (ret == ESP_OK) {
        ret = nvs_get_u16(handle, NVS_KEY_TVOC_BASELINE, &tvoc);
    }

    nvs_close(handle);

    if (ret == ESP_OK) {
        *co2_baseline = co2;
        *tvoc_baseline = tvoc;
        ESP_LOGI(TAG, "基线已恢复: CO2=0x%04X, TVOC=0x%04X", co2, tvoc);
    } else {
        ESP_LOGI(TAG, "NVS 中无基线数据，使用默认值");
    }

    return ret;
}
