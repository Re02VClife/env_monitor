/*
 * main.c — 环境监测仪入口
 * 创建 FreeRTOS 任务：传感器采集 + 按钮监控
 * 显示: ST7789 横屏 320×240 直接帧缓冲渲染（无 LVGL）
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_psram.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_sleep.h"

#include "app_config.h"
#include "app_core.h"
#include "app_display.h"
#include "st7789.h"

static const char *TAG = "main";

/* 按钮引脚 */
#define BTN_REBOOT_GPIO     GPIO_NUM_38   /* 重启: 短按 → esp_restart */
#define BTN_POWER_GPIO      GPIO_NUM_39   /* 开关机: 长按2秒 → 休眠 / 短按唤醒 */
#define POWER_HOLD_MS       2000          /* 关机长按时间 */

/* ----------------------------------------------------------------
 * 按钮任务 — 同时监控重启键(IO38)和开关机键(IO39)
 * ---------------------------------------------------------------- */
static void button_task(void *arg)
{
    (void)arg;
    uint32_t reboot_cnt = 0, power_cnt = 0;
    const uint32_t tick_ms = 50;

    while (1) {
        /* IO38 — 重启键 (100ms 消抖) */
        if (gpio_get_level(BTN_REBOOT_GPIO) == 0) {
            if (++reboot_cnt >= (100 / tick_ms)) {
                ESP_LOGW(TAG, "重启键触发...");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        } else {
            reboot_cnt = 0;
        }

        /* IO39 — 开关机键 (2秒长按休眠) */
        if (gpio_get_level(BTN_POWER_GPIO) == 0) {
            if (++power_cnt >= (POWER_HOLD_MS / tick_ms)) {
                ESP_LOGW(TAG, "关机键长按，进入 deep sleep...");
                vTaskDelay(pdMS_TO_TICKS(200));

                /* 配置 IO39 为 RTC 唤醒源 (低电平唤醒) */
                esp_sleep_enable_ext0_wakeup(BTN_POWER_GPIO, 0);
                esp_deep_sleep_start();
            }
        } else {
            power_cnt = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(tick_ms));
    }
}

/* ----------------------------------------------------------------
 * app_main — 程序入口
 * ---------------------------------------------------------------- */
void app_main(void)
{
    /* 1. 启动日志 */
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  环境监测仪 v1.0");
    ESP_LOGI(TAG, "  芯片: ESP32-S3-WROOM-1-N16R8");
    ESP_LOGI(TAG, "========================================");

    /* 2. 检查 PSRAM */
    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM 大小: %d 字节 (%d MB)", (int)psram_size, (int)(psram_size / (1024 * 1024)));

    /* 3. 初始化 NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 分区需要擦除，正在格式化...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS 初始化完成");

    /* 4. 初始化显示子系统（先亮屏，不依赖传感器） */
    ret = st7789_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ST7789 初始化失败: %s，跳过显示", esp_err_to_name(ret));
    } else {
        app_display_init();
        ESP_LOGI(TAG, "显示子系统初始化完成");
    }

    /* 5. 初始化传感器子系统（失败不阻塞） */
    ret = app_core_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "传感器初始化失败: %s，继续运行", esp_err_to_name(ret));
    }

    /* 6. 创建传感器采集任务 — 核心0, 普通优先级 */
    BaseType_t task_created = xTaskCreatePinnedToCore(
        app_core_sensor_task,
        "sensor",
        8192,
        NULL,
        4,
        NULL,
        0
    );
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "传感器任务创建失败!");
        abort();
    }

    /* 7. 配置按钮 GPIO (IO38 重启, IO39 开关机) */
    gpio_config_t btn_cfg = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BTN_REBOOT_GPIO) | (1ULL << BTN_POWER_GPIO),
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&btn_cfg);

    /* 8. 创建按钮监控任务 */
    task_created = xTaskCreatePinnedToCore(
        button_task,
        "buttons",
        2048,
        NULL,
        1,
        NULL,
        0
    );
    if (task_created != pdPASS) {
        ESP_LOGW(TAG, "按钮任务创建失败，按键不可用");
    }

    ESP_LOGI(TAG, "系统启动完成，进入运行状态");
}
