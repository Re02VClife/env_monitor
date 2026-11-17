/*
 * aht30.c — AHT30 温湿度传感器驱动实现
 * 协议: 发送 0xAC 0x33 0x00 触发测量 → 延时 80ms → 读取 6 字节 → CRC 校验 → 计算
 */
#include "aht30.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "aht30";

#define AHT30_ADDR              0x38
#define AHT30_CMD_INIT          0xE1
#define AHT30_CMD_SOFT_RESET    0xBA
#define AHT30_CMD_TRIGGER       0xAC
#define AHT30_MEAS_WAIT_MS      80

/* 设备句柄（模块内部持有） */
static i2c_master_dev_handle_t dev_handle = NULL;

/* ----------------------------------------------------------------
 * 公共接口
 * ---------------------------------------------------------------- */

esp_err_t aht30_init(i2c_master_bus_handle_t bus_handle)
{
    if (dev_handle != NULL) {
        ESP_LOGW(TAG, "AHT30 已初始化，跳过");
        return ESP_OK;
    }

    /* 添加 I2C 设备 */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AHT30_ADDR,
        .scl_speed_hz    = 100000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C 设备添加失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 检查状态寄存器的校准位 (bit 3) */
    uint8_t status = 0;
    ret = i2c_master_receive(dev_handle, &status, 1, 50);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "读取状态失败: %s，尝试校准", esp_err_to_name(ret));
        goto do_calib;
    }

    if ((status & 0x08) == 0) {
        ESP_LOGW(TAG, "传感器未校准 (status=0x%02X)，执行校准", status);
        goto do_calib;
    }

    ESP_LOGI(TAG, "AHT30 已校准 (status=0x%02X)", status);
    return ESP_OK;

do_calib:
    /* 发送校准命令: 0xE1 0x08 0x00 */
    uint8_t calib_cmd[] = {AHT30_CMD_INIT, 0x08, 0x00};
    ret = i2c_master_transmit(dev_handle, calib_cmd, sizeof(calib_cmd), 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "校准命令失败: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(40));  /* 等待校准完成 */
    ESP_LOGI(TAG, "AHT30 校准完成");
    return ESP_OK;
}

esp_err_t aht30_read(i2c_master_bus_handle_t bus_handle, aht30_data_t *data)
{
    if (dev_handle == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1. 发送触发测量命令 */
    uint8_t cmd[] = {AHT30_CMD_TRIGGER, 0x33, 0x00};
    esp_err_t ret = i2c_master_transmit(dev_handle, cmd, sizeof(cmd), 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "触发测量失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2. 等待测量完成 */
    vTaskDelay(pdMS_TO_TICKS(AHT30_MEAS_WAIT_MS));

    /* 3. 读取 6 字节数据 */
    uint8_t raw[6] = {0};
    ret = i2c_master_receive(dev_handle, raw, 6, 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "读取数据失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 4. AHT30 6 字节响应格式不含 CRC 字节（CRC 仅存在于 7 字节读取模式）
     *    此处不做 CRC 校验，直接解析温湿度数据
     */

    /* 5. 解析温湿度
     * 湿度: raw[1]<<12 | raw[2]<<4 | raw[3]>>4
     *        * 100.0 / 1048576
     * 温度: (raw[3]&0x0F)<<16 | raw[4]<<8 | raw[5]
     *        * 200.0 / 1048576 - 50
     */
    uint32_t hum_raw = ((uint32_t)raw[1] << 12)
                     | ((uint32_t)raw[2] << 4)
                     | ((uint32_t)raw[3] >> 4);
    data->humidity = hum_raw * 100.0f / 1048576.0f;

    uint32_t temp_raw = ((uint32_t)(raw[3] & 0x0F) << 16)
                      | ((uint32_t)raw[4] << 8)
                      | ((uint32_t)raw[5]);
    data->temperature = temp_raw * 200.0f / 1048576.0f - 50.0f;

    return ESP_OK;
}

esp_err_t aht30_soft_reset(i2c_master_bus_handle_t bus_handle)
{
    if (dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t cmd = AHT30_CMD_SOFT_RESET;
    esp_err_t ret = i2c_master_transmit(dev_handle, &cmd, 1, 50);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ret;
}
