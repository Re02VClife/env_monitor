/*
 * app_wifi.h — WiFi 联网、配网与干湿度上报
 *
 * 两种运行模式：
 *   - NVS 里没有 WiFi 凭据：开热点 DryRack-xxxx，手机连上后打开
 *     http://192.168.4.1/ 填 WiFi 密码和 webhook 地址（SoftAP 配网）
 *   - 有凭据：作为 STA 连接路由器，并启动本地 HTTP 服务供查看/控制
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "app_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_WIFI_OFF = 0,
    APP_WIFI_AP_PROV,      /* 配网热点模式 */
    APP_WIFI_CONNECTING,   /* 正在连接路由器 */
    APP_WIFI_CONNECTED,    /* 已联网 */
    APP_WIFI_FAILED,       /* 连接失败 */
} app_wifi_state_t;

/* webhook 报文格式 */
typedef enum {
    WEBHOOK_FMT_JSON = 0,   /* 原始 JSON POST，适合自建服务 */
    WEBHOOK_FMT_TEXT,       /* 企业微信 / 钉钉机器人 */
    WEBHOOK_FMT_FEISHU,     /* 飞书机器人 */
    WEBHOOK_FMT_BARK,       /* Bark (iOS) */
} webhook_fmt_t;

/* 网页请求进入低功耗时的回调（只应置标志，不要在里面直接进深睡） */
typedef void (*app_wifi_sleep_cb_t)(void);

/**
 * @brief 初始化 WiFi：有凭据进 STA，无凭据进 SoftAP 配网
 */
esp_err_t app_wifi_init(void);

/**
 * @brief 是否已经保存过 WiFi 凭据
 */
bool app_wifi_has_credentials(void);

/**
 * @brief 等待联网成功
 * @return ESP_OK 已连接；否则超时/失败
 */
esp_err_t app_wifi_connect(uint32_t timeout_ms);

/**
 * @brief 当前状态
 */
app_wifi_state_t app_wifi_state(void);

/**
 * @brief 当前状态的中文描述（"已联网"/"未联网"/"配网中"）
 */
const char *app_wifi_state_text(void);

/**
 * @brief 启动本地 HTTP 服务（配网页 / 状态页 / API）
 */
esp_err_t app_wifi_start_http_server(void);

/**
 * @brief 把一次读数推送到 webhook
 * @return ESP_OK 服务器返回 2xx
 */
esp_err_t app_wifi_report(const env_reading_t *reading);

/**
 * @brief 注册「网页点了休眠」的回调
 */
void app_wifi_set_sleep_callback(app_wifi_sleep_cb_t cb);

/**
 * @brief 断线后重新发起连接（重置重试计数）
 */
void app_wifi_kick_reconnect(void);

/**
 * @brief 清除 WiFi 凭据（下次启动进入配网模式）
 */
void app_wifi_clear_credentials(void);

/**
 * @brief 关闭 WiFi 与 HTTP 服务（进入深睡前调用）
 */
void app_wifi_stop(void);

#ifdef __cplusplus
}
#endif
