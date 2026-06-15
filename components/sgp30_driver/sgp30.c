/*
 * sgp30.c — SGP30 气体传感器驱动实现
 *
 * SGP30 协议特点:
 *  - I2C 地址固定 0x58
 *  - 所有 2 字节数据后紧跟 1 字节 CRC-8 (多项式 0x31, 初始值 0xFF)
 *  - IAQ 初始化需 15 秒预热（期间 NACK 所有 I2C 通信）
 *  - 基线值可读写，断电保存后可加速再次启动的稳定时间
 */
#include "sgp30.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "sgp30";

#define SGP30_ADDR              0x58

/* SGP30 命令字 */
#define SGP30_CMD_IAQ_INIT          0x2003
#define SGP30_CMD_MEASURE_IAQ       0x2008
#define SGP30_CMD_GET_BASELINE      0x2015
#define SGP30_CMD_SET_BASELINE      0x201E
#define SGP30_CMD_SET_HUMIDITY      0x2061
#define SGP30_CMD_GET_FEATURE_SET   0x202F
#define SGP30_CMD_MEASURE_RAW       0x2032
#define SGP30_CMD_GET_SERIAL_ID     0x2006

/* 设备句柄 */
static i2c_master_dev_handle_t dev_handle = NULL;

/* ----------------------------------------------------------------
 * CRC-8 校验（SGP30: 多项式 0x31, 初始值 0xFF）
 * ---------------------------------------------------------------- */
static uint8_t sgp30_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x31) : (crc << 1);
        }
    }
    return crc;
}

/* ----------------------------------------------------------------
 * 底层 I2C 通信封装
 * ---------------------------------------------------------------- */

/* 发送 2 字节命令（无参数） */
static esp_err_t sgp30_send_cmd(uint16_t cmd)
{
    uint8_t buf[2] = { (cmd >> 8) & 0xFF, cmd & 0xFF };
    return i2c_master_transmit(dev_handle, buf, sizeof(buf), 50);
}

/* 发送命令 + 数据（带 CRC） */
static esp_err_t sgp30_send_cmd_with_data(uint16_t cmd, const uint8_t *data, size_t len)
{
    /* 缓冲区: 2 字节命令 + 数据 */
    uint8_t buf[2 + 16];
    buf[0] = (cmd >> 8) & 0xFF;
    buf[1] = cmd & 0xFF;
    memcpy(buf + 2, data, len);
    return i2c_master_transmit(dev_handle, buf, len + 2, 50);
}

/* 读取指定长度的数据并校验 CRC（每 2 字节跟 1 字节 CRC） */
static esp_err_t sgp30_read_with_crc(uint8_t *dst, size_t dst_len)
{
    /* SGP30 数据格式: 每 2 字节原始数据跟 1 字节 CRC
     * 所以实际读取字节数 = 原始数据字节数 * 1.5
     */
    size_t raw_len = dst_len + (dst_len / 2);  /* 加上 CRC 字节 */
    uint8_t raw[24];
    if (raw_len > sizeof(raw)) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = i2c_master_receive(dev_handle, raw, raw_len, 100);
    if (ret != ESP_OK) return ret;

    /* 提取数据并校验 CRC */
    for (size_t i = 0; i < dst_len; i += 2) {
        size_t raw_idx = i + (i / 2);  /* 跳过 CRC 字节的计算 */
        dst[i]     = raw[raw_idx];
        dst[i + 1] = raw[raw_idx + 1];
        uint8_t expected_crc = raw[raw_idx + 2];
        uint8_t computed_crc = sgp30_crc8(&raw[raw_idx], 2);
        if (expected_crc != computed_crc) {
            ESP_LOGW(TAG, "CRC 校验失败: 期望 0x%02X, 计算 0x%02X", expected_crc, computed_crc);
            /* 不阻塞，继续执行 */
        }
    }
    return ESP_OK;
}

/* ----------------------------------------------------------------
 * 公共接口
 * ---------------------------------------------------------------- */

esp_err_t sgp30_init(i2c_master_bus_handle_t bus_handle)
{
    if (dev_handle != NULL) {
        ESP_LOGW(TAG, "SGP30 已初始化，跳过");
        return ESP_OK;
    }

    esp_err_t ret;

    /* 先用 I2C probe 检测芯片是否在总线上应答 */
    ret = i2c_master_probe(bus_handle, SGP30_ADDR, 200);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "I2C Probe 成功: 在地址 0x%02X 检测到设备", SGP30_ADDR);
    } else {
        /* 探针失败说明芯片不存在或硬件故障，不应继续后续操作 */
        ESP_LOGE(TAG, "I2C Probe 失败: 地址 0x%02X 无设备应答！请检查 SGP30 供电和焊接 (ret=%s)",
                 SGP30_ADDR, esp_err_to_name(ret));
        return (ret == ESP_ERR_NOT_FOUND) ? ESP_ERR_NOT_FOUND : ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SGP30_ADDR,
        .scl_speed_hz    = 100000,
    };
    ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C 设备添加失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 读取 Feature Set 版本确认芯片存在 */
    ret = sgp30_send_cmd(SGP30_CMD_GET_FEATURE_SET);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Feature Set 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(2));

    uint8_t feature[2];
    ret = sgp30_read_with_crc(feature, sizeof(feature));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "读取 Feature Set 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    uint16_t feature_val = (feature[0] << 8) | feature[1];
    ESP_LOGI(TAG, "SGP30 Feature Set: 0x%04X", feature_val);

    /* SGP30 产品 ID: 0x0022 */
    if (feature_val != 0x0022) {
        ESP_LOGW(TAG, "非预期 Feature Set 值 0x%04X（期望 0x0022），但继续运行", feature_val);
    }

    return ESP_OK;
}

esp_err_t sgp30_iaq_init(i2c_master_bus_handle_t bus_handle)
{
    if (dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "启动 IAQ 初始化（等待 15 秒，建立基线）...");
    esp_err_t ret = sgp30_send_cmd(SGP30_CMD_IAQ_INIT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IAQ Init 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* SGP30 上电后首次 IAQ 初始化必须等满 15 秒，让内部基线算法完成初始采样。
     * 注意: 新模块可能提前 ACK 其他命令，但这不代表 IAQ 初始化已完成。
     * 因此不再轮询，直接硬等 15 秒。 */
    vTaskDelay(pdMS_TO_TICKS(15000));
    ESP_LOGI(TAG, "IAQ 初始化完成 (15 秒)");
    return ESP_OK;
}

esp_err_t sgp30_read(i2c_master_bus_handle_t bus_handle, sgp30_data_t *data)
{
    if (dev_handle == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = sgp30_send_cmd(SGP30_CMD_MEASURE_IAQ);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Measure IAQ 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 等待测量完成（典型 12ms） */
    vTaskDelay(pdMS_TO_TICKS(15));

    uint8_t buf[4];  /* CO2[2] + TVOC[2] */
    ret = sgp30_read_with_crc(buf, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "读取 IAQ 数据失败: %s", esp_err_to_name(ret));
        return ret;
    }

    data->co2_ppm  = (buf[0] << 8) | buf[1];
    data->tvoc_ppb = (buf[2] << 8) | buf[3];

    return ESP_OK;
}

esp_err_t sgp30_get_baseline(i2c_master_bus_handle_t bus_handle, sgp30_baseline_t *b)
{
    if (dev_handle == NULL || b == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = sgp30_send_cmd(SGP30_CMD_GET_BASELINE);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t buf[4];
    ret = sgp30_read_with_crc(buf, sizeof(buf));
    if (ret != ESP_OK) return ret;

    b->co2_baseline  = (buf[0] << 8) | buf[1];
    b->tvoc_baseline = (buf[2] << 8) | buf[3];
    return ESP_OK;
}

esp_err_t sgp30_set_baseline(i2c_master_bus_handle_t bus_handle, const sgp30_baseline_t *b)
{
    if (dev_handle == NULL || b == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 构造基线数据: CO2[2] + CRC[1] + TVOC[2] + CRC[1] */
    uint8_t data[6];
    data[0] = (b->co2_baseline >> 8) & 0xFF;
    data[1] = b->co2_baseline & 0xFF;
    data[2] = sgp30_crc8(&data[0], 2);
    data[3] = (b->tvoc_baseline >> 8) & 0xFF;
    data[4] = b->tvoc_baseline & 0xFF;
    data[5] = sgp30_crc8(&data[3], 2);

    ESP_LOGI(TAG, "恢复基线: CO2=0x%04X, TVOC=0x%04X", b->co2_baseline, b->tvoc_baseline);
    return sgp30_send_cmd_with_data(SGP30_CMD_SET_BASELINE, data, sizeof(data));
}

esp_err_t sgp30_set_humidity(i2c_master_bus_handle_t bus_handle, float abs_humidity_gm3)
{
    if (dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 绝对湿度 (g/m³) 转换为传感器刻度:
     * humidity_ticks = abs_humidity * 256
     * 示例: 10.5 g/m³ → 2688 → 0x0A80
     */
    if (abs_humidity_gm3 < 0.0f) abs_humidity_gm3 = 0.0f;
    if (abs_humidity_gm3 > 256.0f) abs_humidity_gm3 = 256.0f;

    uint16_t ticks = (uint16_t)(abs_humidity_gm3 * 256.0f);
    uint8_t data[3];
    data[0] = (ticks >> 8) & 0xFF;
    data[1] = ticks & 0xFF;
    data[2] = sgp30_crc8(data, 2);

    /* 使用带数据的命令发送 */
    uint8_t buf[5];
    buf[0] = (SGP30_CMD_SET_HUMIDITY >> 8) & 0xFF;
    buf[1] = SGP30_CMD_SET_HUMIDITY & 0xFF;
    buf[2] = data[0];
    buf[3] = data[1];
    buf[4] = data[2];

    return i2c_master_transmit(dev_handle, buf, sizeof(buf), 50);
}
