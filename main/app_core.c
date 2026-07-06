/*
 * app_core.c — 传感器采集核心逻辑
 *
 * 状态机:
 *   INIT → WARMUP (SGP30 IAQ 初始化, 15s) → RUNNING (每秒采集)
 *
 * 数据流:
 *   AHT30 → 温湿度 + 绝对湿度补偿 → SGP30 → CO2/TVOC
 *        ↓
 *   app_display_update() → 界面刷新
 *   ws2812_set_co2_level() → LED 颜色指示
 *
 * 基线策略:
 *   不保存、不恢复基线。让 SGP30 每次上电从默认状态自学习，
 *   避免预存基线导致算法过早收敛到 400ppm。
 *   SGP30 会在运行中持续自校准，前 12 小时精度逐步提升。
 */
#include "app_core.h"
#include "app_config.h"
#include "app_display.h"
#include "aht30.h"
#include "sgp30.h"
#include "nvs_storage.h"
#include "ws2812.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>

static const char *TAG = "app_core";

/* I2C 总线句柄 — 双总线物理隔离传感器 */
static i2c_master_bus_handle_t i2c_bus0 = NULL;  /* AHT30 */
static i2c_master_bus_handle_t i2c_bus1 = NULL;  /* SGP30 */

/* SGP30 可用标志（由 app_core_init 设置，sensor_task 读取） */
static bool sgp30_available = false;

/* ----------------------------------------------------------------
 * 绝对湿度计算（用于 SGP30 湿度补偿）
 *
 * 输入:  温度 T (°C), 相对湿度 RH (%)
 * 输出:  绝对湿度 (g/m³)
 *
 * 公式:
 *   饱和蒸气压  es = 6.112 × exp(17.67 × T / (T + 243.5))   [hPa]
 *   实际蒸气压  e  = es × RH / 100
 *   绝对湿度    AH = 216.7 × e / (T + 273.15)              [g/m³]
 * ---------------------------------------------------------------- */
static float compute_abs_humidity(float temperature, float humidity)
{
    float es = 6.112f * expf(17.67f * temperature / (temperature + 243.5f));
    float e  = es * humidity / 100.0f;
    float ah = 216.7f * e / (temperature + 273.15f);
    return ah;
}

/* ----------------------------------------------------------------
 * I2C 总线初始化 — 双总线，物理隔离传感器
 * ---------------------------------------------------------------- */
static esp_err_t i2c_bus_init(void)
{
    /* 总线 0：AHT30 */
    i2c_master_bus_config_t cfg0 = {
        .i2c_port    = I2C_NUM_0,
        .sda_io_num  = I2C0_SDA_IO,
        .scl_io_num  = I2C0_SCL_IO,
        .clk_source  = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg0, &i2c_bus0));
    ESP_LOGI(TAG, "I2C 总线0 初始化完成 (SDA=IO%d, SCL=IO%d)",
             I2C0_SDA_IO, I2C0_SCL_IO);

    /* 总线 1：SGP30 */
    i2c_master_bus_config_t cfg1 = {
        .i2c_port    = I2C_NUM_1,
        .sda_io_num  = I2C1_SDA_IO,
        .scl_io_num  = I2C1_SCL_IO,
        .clk_source  = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg1, &i2c_bus1));
    ESP_LOGI(TAG, "I2C 总线1 初始化完成 (SDA=IO%d, SCL=IO%d)",
             I2C1_SDA_IO, I2C1_SCL_IO);

    return ESP_OK;
}

/* ----------------------------------------------------------------
 * 公共接口
 * ---------------------------------------------------------------- */

esp_err_t app_core_init(void)
{
    ESP_LOGI(TAG, "初始化传感器子系统...");

    /* 1. I2C 总线 */
    ESP_ERROR_CHECK(i2c_bus_init());

    /* 2. NVS 存储 */
    ESP_ERROR_CHECK(nvs_storage_init());

    /* 3. AHT30 温湿度传感器 (I2C 总线0，地址 0x38) */
    esp_err_t ret = aht30_init(i2c_bus0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AHT30 初始化失败: %s，跳过", esp_err_to_name(ret));
    }

    /* 4. SGP30 气体传感器 (I2C 总线1，地址 0x58) */
    ret = sgp30_init(i2c_bus1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SGP30 初始化失败: %s，跳过", esp_err_to_name(ret));
        sgp30_available = false;
    } else {
        sgp30_available = true;
    }

    /* 5. WS2812B LED（失败不中止） */
    ret = ws2812_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WS2812B 初始化失败: %s，跳过", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "传感器子系统初始化完成");
    return ESP_OK;
}

void app_core_sensor_task(void *arg)
{
    (void)arg;
    esp_err_t ret;
    ESP_LOGI(TAG, "传感器任务启动 (core %d)", xPortGetCoreID());

    /* ============================================================
     * 阶段 1: WARMUP — SGP30 IAQ 预热 (15 秒)
     * 不恢复历史基线，让传感器从出厂默认状态自学习
     * ============================================================ */
    bool sgp30_ready = false;

    if (sgp30_available) {
        ESP_LOGI(TAG, "--- 预热阶段 (SGP30) ---");
        ESP_LOGI(TAG, "执行硬件复位 + IAQ 初始化...");

        sgp30_reset(i2c_bus1);              /* 硬件复位，恢复到出厂状态 */
        esp_err_t iaq_ret = sgp30_iaq_init(i2c_bus1);  /* 15 秒预热 */
        if (iaq_ret != ESP_OK) {
            ESP_LOGW(TAG, "SGP30 IAQ 初始化失败，仅采集温湿度");
            sgp30_ready = false;
        } else {
            sgp30_ready = true;
        }
    } else {
        ESP_LOGI(TAG, "--- 跳过预热阶段 (SGP30 不可用) ---");
    }

    ws2812_set_color(LED_COLOR_GREEN);

    /* ============================================================
     * 阶段 2: RUNNING — 定时采集循环
     * 每秒: AHT30 → 湿度补偿 → SGP30 → 更新显示 → LED
     * 每 IAQ_RESET_INTERVAL_H 小时: 重新初始化 SGP30 IAQ，
     *   阻止基线在持续高 CO2 环境中深度收敛到 400ppm
     * ============================================================ */
    ESP_LOGI(TAG, "--- 运行阶段 (IAQ 每 %d 小时复位) ---", IAQ_RESET_INTERVAL_H);
    TickType_t last_wake = xTaskGetTickCount();
    int64_t last_iaq_reset_us = esp_timer_get_time();
    const int64_t iaq_reset_interval_us = (int64_t)IAQ_RESET_INTERVAL_H * 3600 * 1000000;

    while (1) {
        aht30_data_t env = { .temperature = 0.0f, .humidity = 0.0f };
        sgp30_data_t gas  = { .co2_ppm = 0, .tvoc_ppb = 0 };
        float abs_hum = 0.0f;

        ret = aht30_read(i2c_bus0, &env);
        if (ret == ESP_OK) {
            abs_hum = compute_abs_humidity(env.temperature, env.humidity);
            if (sgp30_ready) {
                sgp30_set_humidity(i2c_bus1, abs_hum);
                sgp30_read(i2c_bus1, &gas);
            }
            ESP_LOGI(TAG, "T=%.1f°C  RH=%.1f%%  CO2=%dppm  TVOC=%dppb",
                     env.temperature, env.humidity, gas.co2_ppm, gas.tvoc_ppb);
        } else {
            ESP_LOGW(TAG, "AHT30 读取失败，使用默认值刷新界面");
        }

        app_display_update(env.temperature, env.humidity,
                           gas.co2_ppm, gas.tvoc_ppb);
        if (sgp30_ready) ws2812_set_co2_level(gas.co2_ppm);

        /* ---- 定时 IAQ 复位: 阻止基线深度收敛 ---- */
        if (sgp30_available &&
            (esp_timer_get_time() - last_iaq_reset_us) >= iaq_reset_interval_us) {

            ESP_LOGI(TAG, "--- 定时硬件复位 + IAQ 重启 (每 %d 小时) ---", IAQ_RESET_INTERVAL_H);
            ws2812_set_color(LED_COLOR_BLUE);

            sgp30_reset(i2c_bus1);          /* 硬件复位，清除可能的卡死状态 */
            esp_err_t iaq_ret = sgp30_iaq_init(i2c_bus1);  /* 重新初始化 IAQ */
            sgp30_ready = (iaq_ret == ESP_OK);

            last_iaq_reset_us = esp_timer_get_time();
            ws2812_set_color(LED_COLOR_GREEN);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
    }
}
