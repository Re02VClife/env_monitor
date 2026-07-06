/*
 * app_core.c — 传感器采集核心逻辑
 *
 * 基线策略（智能自动，无需手动干预）:
 *   启动时 → 恢复已保存的基线（维持校准连续性）
 *   运行时 → 仅在 CO2 连续 ≤ 450ppm 超过 1 小时后才保存基线
 *             确保基线只记录「真正的新鲜空气」，不被高浓度污染
 *   不复位  → 传感器持续运行，达到 12 小时最优精度后保持稳定
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

/* SGP30 可用标志 */
static bool sgp30_available = false;

/* ----------------------------------------------------------------
 * 绝对湿度计算（用于 SGP30 湿度补偿）
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

    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(nvs_storage_init());

    esp_err_t ret = aht30_init(i2c_bus0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AHT30 初始化失败: %s，跳过", esp_err_to_name(ret));
    }

    ret = sgp30_init(i2c_bus1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SGP30 初始化失败: %s，跳过", esp_err_to_name(ret));
        sgp30_available = false;
    } else {
        sgp30_available = true;
    }

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
     * 阶段 1: WARMUP — 恢复基线 + IAQ 预热 (15 秒)
     * ============================================================ */
    bool sgp30_ready = false;
    bool baseline_was_restored = false;

    if (sgp30_available) {
        ESP_LOGI(TAG, "--- 预热阶段 (SGP30) ---");

        /* 尝试恢复已保存的基线（仅在新鲜空气中保存过的） */
        uint16_t saved_co2_base = 0, saved_tvoc_base = 0;
        if (nvs_load_sgp30_baseline(&saved_co2_base, &saved_tvoc_base) == ESP_OK
            && saved_co2_base > 0) {
            sgp30_baseline_t baseline = {
                .co2_baseline  = saved_co2_base,
                .tvoc_baseline = saved_tvoc_base,
            };
            sgp30_set_baseline(i2c_bus1, &baseline);
            baseline_was_restored = true;
        } else {
            ESP_LOGI(TAG, "无有效基线，使用出厂默认值（约 12 小时后达到最优精度）");
        }

        esp_err_t iaq_ret = sgp30_iaq_init(i2c_bus1);
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
     * 阶段 2: RUNNING — 连续采集，不复位
     * 智能基线: 仅在确认空气新鲜时自动保存
     * ============================================================ */
    ESP_LOGI(TAG, "--- 运行阶段 (智能基线: CO2≤%dppm 持续%ds 后自动保存) ---",
             BASELINE_CLEAN_CO2_MAX, BASELINE_CLEAN_HOLD_S);
    ESP_LOGI(TAG, "传感器持续运行，不复位，长期稳定");

    TickType_t last_wake = xTaskGetTickCount();
    uint32_t clean_air_seconds = 0;       /* 连续低 CO2 秒数 */
    bool baseline_saved_this_session = false;

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

        /* ---- 智能基线保存: CO2 连续低于阈值超过指定时间才保存 ---- */
        if (sgp30_ready && gas.co2_ppm > 0) {
            if (gas.co2_ppm <= BASELINE_CLEAN_CO2_MAX) {
                clean_air_seconds++;
            } else {
                clean_air_seconds = 0;   /* CO2 升高，计数器清零 */
            }

            /* 达到持续时间要求 → 确认是新鲜空气 → 保存基线 */
            if (clean_air_seconds >= BASELINE_CLEAN_HOLD_S
                && !baseline_saved_this_session) {

                sgp30_baseline_t cur_baseline;
                if (sgp30_get_baseline(i2c_bus1, &cur_baseline) == ESP_OK) {
                    nvs_save_sgp30_baseline(cur_baseline.co2_baseline,
                                            cur_baseline.tvoc_baseline);
                    baseline_saved_this_session = true;
                    ESP_LOGI(TAG, "✅ 智能基线已保存 (CO2≤%dppm 持续 %ds)",
                             BASELINE_CLEAN_CO2_MAX, BASELINE_CLEAN_HOLD_S);
                }
                clean_air_seconds = 0;   /* 保存后清零，本会话不再保存 */
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
    }
}
