/*
 * app_core.c — 温湿度采集与干湿判定
 *
 * SGP30 已从硬件上拆除，这里只保留 AHT30 单传感器链路，
 * 并派生绝对湿度 / 露点 / 晾衣干湿等级。
 */
#include "app_core.h"

#include <math.h>
#include <string.h>

#include "app_config.h"
#include "app_display.h"
#include "aht30.h"
#include "ws2812.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_core";

/* I2C 总线（AHT30 独占一条） */
static i2c_master_bus_handle_t s_bus = NULL;
static bool s_sensor_ok = false;

/* 最近一次有效读数（传感器任务写，HTTP/上报读） */
static portMUX_TYPE s_last_lock = portMUX_INITIALIZER_UNLOCKED;
static env_reading_t s_last;
static bool s_last_valid = false;

/* 进入低功耗前冻结屏幕刷新，防止关屏后又被点亮 */
static volatile bool s_suspend_updates = false;

/* ----------------------------------------------------------------
 * 派生量计算
 * ---------------------------------------------------------------- */

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void derive_metrics(env_reading_t *r)
{
    const float t  = r->temperature;
    const float rh = clampf(r->humidity, 0.0f, 100.0f);

    /* 绝对湿度 AH = 216.7 * e / (T + 273.15)，e 为水汽分压 (hPa) */
    const float es = 6.112f * expf(17.67f * t / (t + 243.5f));
    const float e  = es * rh / 100.0f;
    r->abs_humidity = 216.7f * e / (t + 273.15f);

    /* 露点（Magnus 公式，系数取 WMO 常用值） */
    const float a = 17.62f;
    const float b = 243.12f;
    const float rh_safe = clampf(rh, 1.0f, 100.0f);
    const float gamma = logf(rh_safe / 100.0f) + (a * t) / (b + t);
    r->dew_point = (b * gamma) / (a - gamma);

    /* 晾衣干湿分级 */
    if (rh <= RH_DRY_MAX) {
        r->level = DRY_LEVEL_DRY;
    } else if (rh <= RH_NORMAL_MAX) {
        r->level = DRY_LEVEL_NORMAL;
    } else if (rh <= RH_DAMP_MAX) {
        r->level = DRY_LEVEL_DAMP;
    } else {
        r->level = DRY_LEVEL_WET;
    }

    r->cold = (t < COLD_TEMP_C);
}

/* ----------------------------------------------------------------
 * 公共接口
 * ---------------------------------------------------------------- */

esp_err_t app_core_init(void)
{
    if (s_bus != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port      = I2C_NUM_0,
        .sda_io_num    = I2C_SDA_IO,
        .scl_io_num    = I2C_SCL_IO,
        .clk_source    = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线初始化失败: %s", esp_err_to_name(ret));
        s_bus = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "I2C 初始化完成 (SDA=IO%d, SCL=IO%d)",
             I2C_SDA_IO, I2C_SCL_IO);

    ret = aht30_init(s_bus);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AHT30 初始化失败: %s，温湿度将不可用", esp_err_to_name(ret));
        s_sensor_ok = false;
    } else {
        s_sensor_ok = true;
    }

    ret = ws2812_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WS2812B 初始化失败: %s，指示灯不可用", esp_err_to_name(ret));
    }

    return ESP_OK;
}

esp_err_t app_core_read(env_reading_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_sensor_ok) {
        return ESP_ERR_NOT_FOUND;
    }

    aht30_data_t raw = { 0 };
    esp_err_t ret = aht30_read(s_bus, &raw);
    if (ret != ESP_OK) {
        return ret;
    }

    memset(out, 0, sizeof(*out));
    out->temperature = raw.temperature;
    out->humidity    = raw.humidity;
    derive_metrics(out);
    out->valid = true;

    portENTER_CRITICAL(&s_last_lock);
    s_last = *out;
    s_last_valid = true;
    portEXIT_CRITICAL(&s_last_lock);

    return ESP_OK;
}

esp_err_t app_core_read_average(env_reading_t *out, int samples, int gap_ms)
{
    if (out == NULL || samples <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    float t_sum = 0.0f, h_sum = 0.0f;
    int   ok_cnt = 0;

    for (int i = 0; i < samples; i++) {
        aht30_data_t raw = { 0 };
        if (aht30_read(s_bus, &raw) == ESP_OK) {
            t_sum += raw.temperature;
            h_sum += raw.humidity;
            ok_cnt++;
        }
        if (i + 1 < samples) {
            vTaskDelay(pdMS_TO_TICKS(gap_ms));
        }
    }

    if (ok_cnt == 0) {
        return ESP_FAIL;
    }

    memset(out, 0, sizeof(*out));
    out->temperature = t_sum / (float)ok_cnt;
    out->humidity    = h_sum / (float)ok_cnt;
    derive_metrics(out);
    out->valid = true;

    portENTER_CRITICAL(&s_last_lock);
    s_last = *out;
    s_last_valid = true;
    portEXIT_CRITICAL(&s_last_lock);

    return ESP_OK;
}

bool app_core_get_last(env_reading_t *out)
{
    if (out == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_last_lock);
    bool valid = s_last_valid;
    if (valid) {
        *out = s_last;
    }
    portEXIT_CRITICAL(&s_last_lock);

    return valid;
}

void app_core_sensor_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "采集任务启动 (core %d)，间隔 %d ms",
             xPortGetCoreID(), SAMPLE_INTERVAL_MS);

    TickType_t last_wake = xTaskGetTickCount();
    uint32_t log_div = 0;

    while (1) {
        env_reading_t r = { 0 };

        if (s_suspend_updates) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (app_core_read(&r) == ESP_OK) {
            app_display_update(&r);
            app_core_led_set_level(r.level);

            if (log_div % 10 == 0) {
                ESP_LOGI(TAG, "T=%.1f°C RH=%.1f%% AH=%.1fg/m³ 露点=%.1f°C → %s",
                         r.temperature, r.humidity, r.abs_humidity,
                         r.dew_point, dry_level_text(r.level));
            }
        } else {
            if (log_div % 10 == 0) {
                ESP_LOGW(TAG, "AHT30 读取失败");
            }
        }

        log_div++;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
    }
}

const char *dry_level_text(dry_level_t level)
{
    switch (level) {
    case DRY_LEVEL_DRY:    return "干燥";
    case DRY_LEVEL_NORMAL: return "适宜";
    case DRY_LEVEL_DAMP:   return "偏潮";
    case DRY_LEVEL_WET:    return "潮湿";
    default:               return "未知";
    }
}

const char *dry_level_code(dry_level_t level)
{
    switch (level) {
    case DRY_LEVEL_DRY:    return "dry";
    case DRY_LEVEL_NORMAL: return "normal";
    case DRY_LEVEL_DAMP:   return "damp";
    case DRY_LEVEL_WET:    return "wet";
    default:               return "unknown";
    }
}

esp_err_t app_core_led_set_level(dry_level_t level)
{
    switch (level) {
    case DRY_LEVEL_DRY:
    case DRY_LEVEL_NORMAL:
        return ws2812_set_color(LED_COLOR_GREEN);
    case DRY_LEVEL_DAMP:
        return ws2812_set_color(LED_COLOR_YELLOW);
    case DRY_LEVEL_WET:
        return ws2812_set_color(LED_COLOR_RED);
    default:
        return ws2812_set_color(LED_COLOR_OFF);
    }
}

void app_core_suspend_updates(void)
{
    s_suspend_updates = true;
}
