/*
 * app_power.c — 低功耗模式与唤醒管理
 */
#include "app_power.h"

#include <stdbool.h>

#include "app_config.h"
#include "app_core.h"
#include "app_display.h"
#include "app_wifi.h"
#include "esp_bit_defs.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ws2812.h"

static const char *TAG = "power";

/* IDF v6 起 esp_sleep_get_wakeup_cause() 被 esp_sleep_get_wakeup_causes() 取代，
 * 后者返回 BIT(cause) 位图；这里统一成位图形式，兼容 5.x / 6.x。 */
static uint32_t wake_cause_bitmap(void)
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    return esp_sleep_get_wakeup_causes();
#else
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    return (cause == ESP_SLEEP_WAKEUP_UNDEFINED) ? 0UL : BIT(cause);
#endif
}

boot_mode_t app_power_detect_boot_mode(void)
{
    uint32_t causes = wake_cause_bitmap();

    if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
        /* 定时器唤醒：低功耗周期，静默采集后继续睡 */
        return BOOT_MODE_TIMER_CYCLE;
    }
    /* BOOT 键唤醒 / 冷启动 / 复位 → 正常模式 */
    return BOOT_MODE_NORMAL;
}

const char *app_power_wakeup_cause_text(void)
{
    uint32_t causes = wake_cause_bitmap();

    if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
        return "定时器唤醒（低功耗周期）";
    }
    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT0)) {
        return "BOOT 键唤醒";
    }
    if (causes == 0 || (causes & BIT(ESP_SLEEP_WAKEUP_UNDEFINED))) {
        return "上电 / 复位启动";
    }
    return "其他来源唤醒";
}

void app_power_screen_off(void)
{
    app_display_off();
}

void app_power_screen_on(void)
{
    app_display_on();
}

void app_power_enter_sleep(void)
{
    ESP_LOGW(TAG, "==========================================");
    ESP_LOGW(TAG, "  进入低功耗模式");
    ESP_LOGW(TAG, "  屏幕关闭，每 %d 分钟自动唤醒采集一次", SLEEP_INTERVAL_MIN);
    ESP_LOGW(TAG, "  按 BOOT 键或复位可立即唤醒");
    ESP_LOGW(TAG, "==========================================");

    /* 1. 先冻结采集任务，避免关屏后又被刷新点亮。
     *    sensor 一轮最长约 150ms（80ms 测量 + 61ms 刷屏），这里留足余量。 */
    app_core_suspend_updates();
    vTaskDelay(pdMS_TO_TICKS(300));

    /* 2. 关显示（黑屏 + DISPOFF + 关背光） */
    app_display_off();

    /* 3. 关指示灯 */
    ws2812_set_color(LED_COLOR_OFF);

    /* 4. 关 WiFi 与 HTTP 服务 */
    app_wifi_stop();

    vTaskDelay(pdMS_TO_TICKS(150));

    /* 5. 定时唤醒 */
    esp_err_t err = esp_sleep_enable_timer_wakeup(SLEEP_INTERVAL_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "定时唤醒配置失败: %s", esp_err_to_name(err));
    }

    /* 6. BOOT 键唤醒（对话中的「boot 唤醒」）
     *    ESP32-S3 只有 GPIO0~21 属于 RTC IO，能作为深睡唤醒源。
     *    即使这一步失败也保留了定时唤醒，绝不允许「没有任何唤醒源」地睡下去。 */
    err = esp_sleep_enable_ext0_wakeup(WAKE_BOOT_GPIO, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d 不能作为深睡唤醒源 (%s)，仅保留定时唤醒",
                 WAKE_BOOT_GPIO, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "BOOT 键 (GPIO%d) 已设为唤醒源", WAKE_BOOT_GPIO);
    }

    ESP_LOGI(TAG, "即将进入深睡...");
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_deep_sleep_start();

    /* 不会执行到这里 */
    ESP_LOGE(TAG, "深睡启动失败");
}
