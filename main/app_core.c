/*
 * app_core.c — 传感器采集核心逻辑
 *
 * 状态机:
 *   INIT → WARMUP (SGP30 IAQ 初始化, 15s) → RUNNING (每秒采集)
 *
 * 数据流:
 *   AHT30 → 温湿度 + 绝对湿度补偿 → SGP30 → CO2/TVOC
 *        ↓
 *   app_display_update() → LVGL 界面刷新
 *   ws2812_set_co2_level() → LED 颜色指示
 *   每 60s → nvs_save_sgp30_baseline()
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

/* I2C 总线句柄 — 两个传感器共用一条 I²C 总线 */
static i2c_master_bus_handle_t i2c_bus = NULL;

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
 * I2C 总线初始化 — 单总线，两个传感器共用
 * ---------------------------------------------------------------- */
static esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port    = I2C_NUM_0,
        .sda_io_num  = I2C_SDA_IO,
        .scl_io_num  = I2C_SCL_IO,
        .clk_source  = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c_bus));
    ESP_LOGI(TAG, "I2C 总线初始化完成 (SDA=IO%d, SCL=IO%d)",
             I2C_SDA_IO, I2C_SCL_IO);
    return ESP_OK;
}

/* ----------------------------------------------------------------
 * 公共接口
 * ---------------------------------------------------------------- */

esp_err_t app_core_init(void)
{
    ESP_LOGI(TAG, "初始化传感器子系统...");

    /* 1. I2C 总线（AHT30+SGP30 共用） */
    ESP_ERROR_CHECK(i2c_bus_init());

    /* 2. NVS 存储 */
    ESP_ERROR_CHECK(nvs_storage_init());

    /* 3. AHT30 温湿度传感器 (I2C 地址 0x38) */
    esp_err_t ret = aht30_init(i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AHT30 初始化失败: %s，跳过", esp_err_to_name(ret));
    }

    /* 4. SGP30 气体传感器 (I2C 地址 0x58，与 AHT30 同总线) */
    ret = sgp30_init(i2c_bus);
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
     * 仅当 sgp30_init() 成功后才执行
     * ============================================================ */
    bool sgp30_ready = false;

    if (sgp30_available) {
        ESP_LOGI(TAG, "--- 预热阶段 (SGP30) ---");

        /* 尝试从 NVS 恢复基线 */
        uint16_t saved_co2_base = 0, saved_tvoc_base = 0;
        sgp30_baseline_t baseline;
        if (nvs_load_sgp30_baseline(&saved_co2_base, &saved_tvoc_base) == ESP_OK) {
            baseline.co2_baseline  = saved_co2_base;
            baseline.tvoc_baseline = saved_tvoc_base;
            sgp30_set_baseline(i2c_bus, &baseline);
        } else {
            ESP_LOGI(TAG, "无历史基线，传感器将使用默认值（约 12 小时后自动达到最佳精度）");
        }

        /* 执行 SGP30 IAQ 初始化（内部阻塞 15 秒） */
        ESP_LOGI(TAG, "SGP30 IAQ 初始化中...");
        esp_err_t iaq_ret = sgp30_iaq_init(i2c_bus);
        if (iaq_ret != ESP_OK) {
            ESP_LOGW(TAG, "SGP30 IAQ 初始化失败，仅采集温湿度");
            sgp30_ready = false;
        } else {
            sgp30_ready = true;
        }
    } else {
        ESP_LOGI(TAG, "--- 跳过预热阶段 (SGP30 不可用) ---");
    }

    /* 初始化完成后 LED 变绿 */
    ws2812_set_color(LED_COLOR_GREEN);

    /* ============================================================
     * 阶段 2: RUNNING — 定时采集循环
     * 每秒: 采集 AHT30 → 湿度补偿 → 采集 SGP30 → 更新显示 → 更新 LED
     * 每 60 秒: 保存 SGP30 基线到 NVS
     * ============================================================ */
    ESP_LOGI(TAG, "--- 运行阶段 ---");
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t tick_count  = 0;

    while (1) {
        /* ---- 采集传感器（失败用默认值，不阻塞界面刷新）---- */
        aht30_data_t env = { .temperature = 0.0f, .humidity = 0.0f };
        sgp30_data_t gas  = { .co2_ppm = 0, .tvoc_ppb = 0 };
        float abs_hum = 0.0f;

        ret = aht30_read(i2c_bus, &env);
        if (ret == ESP_OK) {
            abs_hum = compute_abs_humidity(env.temperature, env.humidity);
            if (sgp30_ready) {
                sgp30_set_humidity(i2c_bus, abs_hum);
                sgp30_read(i2c_bus, &gas);
            }
            ESP_LOGI(TAG, "T=%.1f°C  RH=%.1f%%  CO2=%dppm  TVOC=%dppb",
                     env.temperature, env.humidity, gas.co2_ppm, gas.tvoc_ppb);
        } else {
            ESP_LOGW(TAG, "AHT30 读取失败，使用默认值刷新界面");
        }

        /* ---- 更新仪表盘（传感器失败也刷新）---- */
        app_display_update(env.temperature, env.humidity,
                           gas.co2_ppm, gas.tvoc_ppb);

        /* ---- 更新 WS2812B LED 颜色 ---- */
        if (sgp30_ready) ws2812_set_co2_level(gas.co2_ppm);

        /* ---- 每 60 秒保存一次 SGP30 基线 ---- */
        tick_count++;
        if (sgp30_ready && tick_count % BASELINE_SAVE_INTERVAL_S == 0) {
            sgp30_baseline_t cur_baseline;
            if (sgp30_get_baseline(i2c_bus, &cur_baseline) == ESP_OK) {
                nvs_save_sgp30_baseline(cur_baseline.co2_baseline,
                                        cur_baseline.tvoc_baseline);
            }
        }

        /* 等待下一采样周期 */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
    }
}
