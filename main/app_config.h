/*
 * app_config.h — 智能干湿提醒衣架 集中配置
 *
 * 引脚、阈值、联网与低功耗参数集中在此。
 * 板型相关的引脚由 menuconfig 中的 "Dry Rack 干湿衣架" 菜单选择。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "sdkconfig.h"
#include "hal/gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 一、板型相关引脚（由 menuconfig 切换）
 * ================================================================ */

#if CONFIG_DRYRACK_BOARD_FREENOVE
/* Freenove ESP32-S3 WROOM：单 I2C 总线 IO1/IO2，LCD 用 IO42 避开 PSRAM */
#define I2C_SCL_IO          GPIO_NUM_2
#define I2C_SDA_IO          GPIO_NUM_1
#else
/* 原 ESP32-S3-WROOM-1 板：I2C 总线 IO5/IO6 */
#define I2C_SCL_IO          GPIO_NUM_5
#define I2C_SDA_IO          GPIO_NUM_6
#endif

/* I2C 参数 */
#define I2C_CLK_SPEED_HZ    100000

/* ================================================================
 * 二、按键
 * ================================================================ */

#define BTN_REBOOT_GPIO     GPIO_NUM_38   /* 短按 → 重启 */
#define BTN_POWER_GPIO      GPIO_NUM_39   /* 长按 2 秒 → 进入低功耗 */
#define POWER_HOLD_MS       2000
#define BTN_SCAN_MS         50

/* 深睡唤醒用的 BOOT 键（RTC IO，可在深睡中响应） */
#define WAKE_BOOT_GPIO      ((gpio_num_t)CONFIG_DRYRACK_BOOT_GPIO)

/* ================================================================
 * 三、采样与干湿分级
 * ================================================================ */

#define SAMPLE_INTERVAL_MS          1000    /* 正常模式采样间隔 */
#define SAMPLE_AVG_COUNT            3       /* 低功耗唤醒时取几次平均 */
#define SAMPLE_AVG_GAP_MS           900     /* AHT30 单次测量约 80ms，间隔留足 */

/* 相对湿度分级阈值（%RH），针对晾衣场景 */
#define RH_DRY_MAX          45.0f   /* ≤45  干燥，衣物易干 */
#define RH_NORMAL_MAX       62.0f   /* 45~62 适宜 */
#define RH_DAMP_MAX         78.0f   /* 62~78 偏潮 */
                                    /* >78   潮湿，不易干/易返潮 */

/* 低于此温度时提示「低温慢干」 */
#define COLD_TEMP_C         12.0f

/* ================================================================
 * 四、NVS 命名空间与键名
 * ================================================================ */

#define NVS_NAMESPACE           "dryrack"
#define NVS_KEY_WIFI_SSID       "wifi_ssid"
#define NVS_KEY_WIFI_PASS       "wifi_pass"
#define NVS_KEY_WEBHOOK_URL     "hook_url"
#define NVS_KEY_WEBHOOK_FMT     "hook_fmt"
#define NVS_KEY_REPORT_EN       "report_en"

/* ================================================================
 * 五、WiFi 与配网
 * ================================================================ */

#define WIFI_AP_SSID_PREFIX     "DryRack-"     /* 后面拼 MAC 后 4 位 */
#define WIFI_AP_PASSWORD        "dryrack123"   /* 配网热点密码，≥8 位 */
#define WIFI_AP_CHANNEL         6

#define WIFI_STA_CONNECT_MS     20000   /* 正常模式连接超时 */
#define WIFI_QUICK_CONNECT_MS   12000   /* 低功耗周期连接超时（省电） */

/* ================================================================
 * 六、联网上报
 * ================================================================ */

#define REPORT_INTERVAL_S       CONFIG_DRYRACK_REPORT_INTERVAL_S
#define WEBHOOK_TIMEOUT_MS      8000
#define WEBHOOK_BODY_MAX        512

/* ================================================================
 * 七、低功耗
 * ================================================================ */

#define SLEEP_INTERVAL_MIN      CONFIG_DRYRACK_SLEEP_INTERVAL_MIN
#define SLEEP_INTERVAL_US       ((uint64_t)SLEEP_INTERVAL_MIN * 60ULL * 1000000ULL)

/* ================================================================
 * 八、WS2812B 指示灯
 * ================================================================ */

#define WS2812_GPIO             GPIO_NUM_48
#define WS2812_NUM_LEDS         1

#ifdef __cplusplus
}
#endif
