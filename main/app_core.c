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
#include "esp_timer.h"
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
 * 预计干衣时间
 *
 * 衣物水分蒸发速率与「剩余含水量 × 蒸发驱动力」成正比，所以在环境
 * 基本稳定的条件下，晾晒过程中环境相对湿度会按指数衰减：
 *
 *     RH(t) = RH_floor + A · e^(-k·t)
 *
 * 两条估算路径：
 *   1. 有足够历史（≥10 点）且湿度确实在下降
 *      → 对 ln(RH - RH_floor) 做最小二乘拟合求 k，再外推到目标湿度
 *   2. 历史不足（刚上电 / 低功耗刚唤醒 / 湿度还没开始降）
 *      → 用饱和水汽压差 VPD 做物理估算，干燥时间近似反比于 VPD
 *
 * VPD = es(T) · (1 - RH/100)，单位 hPa
 * 经验常数按「25°C / 50%RH 约 6.5 小时」标定。
 * ---------------------------------------------------------------- */

#define ETA_HIST_SIZE           96                        /* 历史缓冲点数 */
#define ETA_SAMPLE_INTERVAL_MS  (60 * 1000)               /* 每 60 秒记一点 */
#define ETA_MIN_FIT_POINTS      10                        /* 拟合最少点数 */
#define ETA_BASE_MARGIN         5.0f                      /* 环境基线往下留的余量 */
#define ETA_GOAL_MARGIN         2.0f                      /* 判定「已干」的余量 */
#define ETA_VPD_K               103.0f                    /* t(h) = K / VPD(hPa) */
#define ETA_MAX_MIN             (48 * 60)                 /* 超过 48 小时就不报 */
#define ETA_RISE_RESET          6.0f                      /* 湿度突升多少算「刚挂衣服」 */

typedef struct {
    int64_t ts_ms;
    float   rh;
    float   temp;
} eta_point_t;

static eta_point_t s_hist[ETA_HIST_SIZE];
static int     s_hist_count   = 0;
static int     s_hist_head    = 0;
static int64_t s_hist_last_ms = 0;
static bool    s_eta_dirty    = true;
static int     s_eta_cached   = -1;
static bool    s_eta_by_trend = false;

/* 取历史中第 i 个点（0 为最旧） */
static const eta_point_t *eta_at(int i)
{
    if (i < 0 || i >= s_hist_count) {
        return NULL;
    }
    int idx = (s_hist_head - s_hist_count + i) % ETA_HIST_SIZE;
    if (idx < 0) {
        idx += ETA_HIST_SIZE;
    }
    return &s_hist[idx];
}

static void eta_record(float rh, float temp)
{
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (s_hist_count > 0 && (now_ms - s_hist_last_ms) < ETA_SAMPLE_INTERVAL_MS) {
        return;
    }

    /* 湿度突然大幅上升，多半是刚挂上湿衣服：丢掉旧曲线重新记录 */
    if (s_hist_count > 0) {
        const eta_point_t *prev = eta_at(s_hist_count - 1);
        if (prev != NULL && (rh - prev->rh) > ETA_RISE_RESET) {
            ESP_LOGI(TAG, "湿度突升 %.1f→%.1f%%，判定为挂衣，重置干衣预测",
                     prev->rh, rh);
            s_hist_count = 0;
            s_hist_head  = 0;
        }
    }

    s_hist[s_hist_head].ts_ms = now_ms;
    s_hist[s_hist_head].rh    = rh;
    s_hist[s_hist_head].temp  = temp;

    s_hist_head = (s_hist_head + 1) % ETA_HIST_SIZE;
    if (s_hist_count < ETA_HIST_SIZE) {
        s_hist_count++;
    }

    s_hist_last_ms = now_ms;
    s_eta_dirty    = true;
}

/* 对 ln(RH - floor) 做最小二乘，返回衰减常数 k（1/小时） */
static bool eta_fit_decay(float floor_rh, float *k_out)
{
    if (s_hist_count < ETA_MIN_FIT_POINTS) {
        return false;
    }

    const eta_point_t *first = eta_at(0);
    if (first == NULL) {
        return false;
    }

    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int    n = 0;

    for (int i = 0; i < s_hist_count; i++) {
        const eta_point_t *p = eta_at(i);
        if (p == NULL) {
            continue;
        }

        float y = p->rh - floor_rh;
        if (y <= 0.8f) {
            continue;   /* 太贴近基线，取对数没有意义 */
        }

        double t  = (double)(p->ts_ms - first->ts_ms) / 3600000.0;   /* 小时 */
        double ly = log((double)y);

        sx  += t;
        sy  += ly;
        sxx += t * t;
        sxy += t * ly;
        n++;
    }

    if (n < ETA_MIN_FIT_POINTS) {
        return false;
    }

    double den = (double)n * sxx - sx * sx;
    if (fabs(den) < 1e-9) {
        return false;
    }

    double slope = ((double)n * sxy - sx * sy) / den;

    /* 斜率必须明显为负，才算真的在指数下降 */
    if (slope > -1e-4) {
        return false;
    }

    *k_out = (float)(-slope);
    return true;
}

/* 返回预计剩余分钟数，<0 表示无法估算 */
static int eta_estimate(float rh, float temp, bool *by_trend)
{
    *by_trend = false;

    /* ---- 1. 历史最低湿度作为环境基线 ---- */
    float rh_min = rh;
    for (int i = 0; i < s_hist_count; i++) {
        const eta_point_t *p = eta_at(i);
        if (p != NULL && p->rh < rh_min) {
            rh_min = p->rh;
        }
    }

    float floor_rh = rh_min - ETA_BASE_MARGIN;
    if (floor_rh < 10.0f) {
        floor_rh = 10.0f;
    }

    /* ---- 2. 指数衰减外推 ---- */
    float k = 0.0f;
    if (eta_fit_decay(floor_rh, &k) && k > 0.0f) {
        float goal_rh = rh_min + ETA_GOAL_MARGIN;
        float cur = rh - floor_rh;
        float tgt = goal_rh - floor_rh;

        if (tgt <= 0.5f) {
            tgt = 0.5f;
        }
        if (cur <= tgt) {
            return 0;   /* 已经回到基线，按「干了」算 */
        }

        float hours = logf(cur / tgt) / k;
        if (hours >= 0.0f && hours <= (float)ETA_MAX_MIN / 60.0f) {
            *by_trend = true;
            return (int)(hours * 60.0f);
        }
    }

    /* ---- 3. VPD 物理兜底 ---- */
    const float es = 6.112f * expf(17.67f * temp / (temp + 243.5f));
    float vpd = es * (1.0f - rh / 100.0f);
    if (vpd < 0.5f) {
        vpd = 0.5f;
    }

    float hours = ETA_VPD_K / vpd;
    if (hours > (float)ETA_MAX_MIN / 60.0f) {
        return -1;
    }

    return (int)(hours * 60.0f);
}

static void eta_update(float rh, float temp, env_reading_t *out)
{
    eta_record(rh, temp);

    if (s_eta_dirty) {
        s_eta_cached   = eta_estimate(rh, temp, &s_eta_by_trend);
        s_eta_dirty    = false;
    }

    out->eta_min      = s_eta_cached;
    out->eta_by_trend = s_eta_by_trend;
}

void app_core_eta_reset(void)
{
    s_hist_count   = 0;
    s_hist_head    = 0;
    s_hist_last_ms = 0;
    s_eta_dirty    = true;
    s_eta_cached   = -1;
    s_eta_by_trend = false;
    ESP_LOGI(TAG, "干衣时间历史已重置");
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
    eta_update(out->humidity, out->temperature, out);
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
    eta_update(out->humidity, out->temperature, out);
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
