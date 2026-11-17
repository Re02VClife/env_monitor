/*
 * main.c — 智能干湿提醒衣架 入口
 *
 * 两条启动路径，由唤醒原因决定：
 *
 *   1. 定时器唤醒（低功耗周期）
 *      → 不点亮屏幕，采集一次温湿度 → 连 WiFi 推送 webhook → 继续深睡
 *
 *   2. 上电 / 复位 / BOOT 键唤醒（正常模式）
 *      → 点亮屏幕显示温湿度，WiFi（或配网热点），按键与网页控制
 *
 * 按键：
 *   IO38 短按 100ms      重启
 *   IO39 长按 2s         进入低功耗（关屏 + 30 分钟周期采集）
 *   BOOT 长按 8s         清除 WiFi 配置并重启（重新配网）
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "app_core.h"
#include "app_display.h"
#include "app_power.h"
#include "app_wifi.h"
#include "st7789.h"

static const char *TAG = "main";

#define BOOT_CLEAR_HOLD_MS   8000    /* BOOT 长按清配置 */
#define RECONNECT_GUARD_MS   60000   /* 断线重连检查间隔 */

/* 网页 /api/sleep 只置这个标志，由上报任务真正执行休眠 */
static volatile bool s_sleep_request = false;

static void on_sleep_request(void)
{
    s_sleep_request = true;
}

/* ================================================================
 * 低功耗周期：静默采集一次并上报
 * ================================================================ */

static void run_timer_cycle(void)
{
    ESP_LOGI(TAG, "低功耗周期启动：静默采集一次");

    esp_err_t ret = app_core_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "传感器初始化失败: %s", esp_err_to_name(ret));
    }

    env_reading_t reading = { 0 };
    bool have_data = (app_core_read_average(&reading, SAMPLE_AVG_COUNT,
                                            SAMPLE_AVG_GAP_MS) == ESP_OK);
    if (have_data) {
        ESP_LOGI(TAG, "采集结果: T=%.1f°C RH=%.1f%% 露点=%.1f°C → %s",
                 reading.temperature, reading.humidity,
                 reading.dew_point, dry_level_text(reading.level));
    } else {
        ESP_LOGW(TAG, "采集失败，本轮不推送数据");
    }

    if (app_wifi_has_credentials()) {
        if (app_wifi_init() == ESP_OK &&
            app_wifi_connect(WIFI_QUICK_CONNECT_MS) == ESP_OK) {
            /* 等 DHCP/DNS 就绪再发请求 */
            vTaskDelay(pdMS_TO_TICKS(500));
            if (have_data) {
                app_wifi_report(&reading);
            }
        } else {
            ESP_LOGW(TAG, "本次联网失败，跳过上报");
        }
    } else {
        ESP_LOGW(TAG, "尚未配置 WiFi，本周期不上报");
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    app_power_enter_sleep();   /* 不返回 */
}

/* ================================================================
 * 正常模式的任务
 * ================================================================ */

/* 后台连接 WiFi，避免阻塞界面刷新 */
static void net_task(void *arg)
{
    (void)arg;

    if (app_wifi_state() == APP_WIFI_AP_PROV) {
        app_display_set_net(DISPLAY_NET_PROV);
        ESP_LOGW(TAG, "处于配网模式：手机连接热点后打开 192.168.4.1");
        vTaskDelete(NULL);
        return;
    }

    app_display_set_net(DISPLAY_NET_OFFLINE);

    if (app_wifi_connect(WIFI_STA_CONNECT_MS) == ESP_OK) {
        app_display_set_net(DISPLAY_NET_ONLINE);
        ESP_LOGI(TAG, "已联网，网页查看: http://<设备IP>/");
    } else {
        app_display_set_net(DISPLAY_NET_OFFLINE);
        ESP_LOGW(TAG, "联网失败，继续以本地模式运行");
    }

    vTaskDelete(NULL);
}

/* 定时上报 + 断线重连 + 响应网页休眠指令 */
static void report_task(void *arg)
{
    (void)arg;
    uint32_t elapsed_ms = 0;

    while (1) {
        /* 网页请求休眠：等 HTTP 响应发完再进深睡 */
        if (s_sleep_request) {
            s_sleep_request = false;
            ESP_LOGW(TAG, "执行网页下发的休眠指令");
            vTaskDelay(pdMS_TO_TICKS(800));
            app_power_enter_sleep();   /* 不返回 */
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
        elapsed_ms += 1000;

        /* 每 60 秒尝试恢复连接 */
        if (elapsed_ms % RECONNECT_GUARD_MS == 0) {
            if (app_wifi_state() == APP_WIFI_FAILED) {
                ESP_LOGI(TAG, "尝试重新连接 WiFi...");
                app_wifi_kick_reconnect();
                app_display_set_net(DISPLAY_NET_OFFLINE);
            } else if (app_wifi_state() == APP_WIFI_CONNECTED) {
                app_display_set_net(DISPLAY_NET_ONLINE);
            }
        }

        /* 到点上报 */
        if (elapsed_ms >= (uint32_t)REPORT_INTERVAL_S * 1000) {
            elapsed_ms = 0;
            if (app_wifi_state() != APP_WIFI_CONNECTED) {
                continue;
            }
            env_reading_t r = { 0 };
            if (app_core_get_last(&r)) {
                app_wifi_report(&r);
            }
        }
    }
}

/* 按键扫描 */
static void button_task(void *arg)
{
    (void)arg;

    uint32_t reboot_cnt = 0;
    uint32_t power_cnt  = 0;
    uint32_t boot_cnt   = 0;

    /* 如果本次是被 BOOT 键从深睡唤醒的，按键可能还按着，
     * 先等它松开，避免误触发「长按清配置」。 */
    for (int i = 0; i < 60 && gpio_get_level(WAKE_BOOT_GPIO) == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    while (1) {
        /* IO38 —— 重启 */
        if (gpio_get_level(BTN_REBOOT_GPIO) == 0) {
            if (++reboot_cnt >= (100 / BTN_SCAN_MS)) {
                ESP_LOGW(TAG, "重启键按下，重启设备");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        } else {
            reboot_cnt = 0;
        }

        /* IO39 —— 长按进入低功耗 */
        if (gpio_get_level(BTN_POWER_GPIO) == 0) {
            if (++power_cnt >= (POWER_HOLD_MS / BTN_SCAN_MS)) {
                ESP_LOGW(TAG, "开关机键长按，进入低功耗模式");
                power_cnt = 0;
                vTaskDelay(pdMS_TO_TICKS(100));
                app_power_enter_sleep();   /* 不返回 */
            }
        } else {
            power_cnt = 0;
        }

        /* BOOT —— 长按清除 WiFi 配置 */
        if (gpio_get_level(WAKE_BOOT_GPIO) == 0) {
            if (++boot_cnt >= (BOOT_CLEAR_HOLD_MS / BTN_SCAN_MS)) {
                ESP_LOGW(TAG, "BOOT 长按 %d 秒，清除 WiFi 配置并重启", BOOT_CLEAR_HOLD_MS / 1000);
                app_wifi_clear_credentials();
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else {
            boot_cnt = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_SCAN_MS));
    }
}

/* ================================================================
 * 正常模式入口
 * ================================================================ */

static void run_normal_mode(void)
{
    /* ---- 1. 显示 ---- */
    if (st7789_init() == ESP_OK) {
        app_display_init();
    } else {
        ESP_LOGW(TAG, "显示屏初始化失败，跳过显示");
    }

    /* ---- 2. 传感器 ---- */
    esp_err_t ret = app_core_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "传感器初始化失败: %s", esp_err_to_name(ret));
    }

    /* ---- 3. 采集任务（刷新界面与指示灯） ---- */
    if (xTaskCreatePinnedToCore(app_core_sensor_task, "sensor", 6144,
                                NULL, 4, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "采集任务创建失败");
    }

    /* ---- 4. WiFi ---- */
    if (app_wifi_init() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi 初始化失败，进入纯本地模式");
        app_display_set_net(DISPLAY_NET_OFFLINE);
    } else {
        app_wifi_set_sleep_callback(on_sleep_request);
        app_wifi_start_http_server();
        if (xTaskCreatePinnedToCore(net_task, "net", 4096, NULL, 3, NULL, 0) != pdPASS) {
            ESP_LOGW(TAG, "联网任务创建失败");
        }
    }

    /* ---- 5. 按键 GPIO ---- */
    gpio_config_t btn_cfg = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BTN_REBOOT_GPIO) | (1ULL << BTN_POWER_GPIO) |
                        (1ULL << WAKE_BOOT_GPIO),
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&btn_cfg);

    if (xTaskCreatePinnedToCore(button_task, "buttons", 3072,
                                NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "按键任务创建失败，按键不可用");
    }

    /* ---- 6. 上报任务 ---- */
    if (xTaskCreatePinnedToCore(report_task, "report", 5120,
                                NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "上报任务创建失败，将不会定时推送");
    }

    ESP_LOGI(TAG, "正常模式启动完成");
}

/* ================================================================
 * app_main
 * ================================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "  智能干湿提醒衣架 v2.0");
    ESP_LOGI(TAG, "  唤醒原因: %s", app_power_wakeup_cause_text());
    ESP_LOGI(TAG, "==========================================");

    /* NVS 是所有配置的基础，必须最先就绪 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 分区需要擦除，正在格式化...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    boot_mode_t mode = app_power_detect_boot_mode();

    if (mode == BOOT_MODE_TIMER_CYCLE) {
        run_timer_cycle();   /* 不返回 */
        return;
    }

    run_normal_mode();
}
