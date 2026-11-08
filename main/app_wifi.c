/*
 * app_wifi.c — WiFi 联网、SoftAP 配网、本地 HTTP 服务与 webhook 上报
 *
 * 配网流程：
 *   1. 设备启动时读 NVS，没有 ssid → 开热点 DryRack-xxxx（密码 dryrack123）
 *   2. 手机连上热点，浏览器打开 http://192.168.4.1/ 填写 WiFi 与通知地址
 *   3. 保存到 NVS 后自动重启，之后按 STA 模式连接路由器
 *
 * 联网后：
 *   - 每 REPORT_INTERVAL_S 秒把温湿度推送到 webhook
 *   - 局域网内可直接访问设备 IP 查看实时数据或远程下发休眠指令
 */
#include "app_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "app_core.h"
#include "nvs_storage.h"

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "wifi";

#define STA_MAX_RETRY       6

#define BIT_GOT_IP          BIT0
#define BIT_FAIL            BIT1

static EventGroupHandle_t s_evt = NULL;
static volatile app_wifi_state_t s_state = APP_WIFI_OFF;
static int s_retry = 0;
static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap  = NULL;
static httpd_handle_t s_httpd = NULL;
static bool s_wifi_started = false;
static app_wifi_sleep_cb_t s_sleep_cb = NULL;
static char s_ap_ssid[33] = { 0 };

/* ================================================================
 * 小工具
 * ================================================================ */

static const char *device_id(void)
{
    static char id[8] = { 0 };
    if (id[0] == '\0') {
        uint8_t mac[6] = { 0 };
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(id, sizeof(id), "%02X%02X%02X", mac[3], mac[4], mac[5]);
    }
    return id;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* application/x-www-form-urlencoded 解码 */
static void url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t oi = 0;
    for (size_t i = 0; src[i] != '\0' && oi + 1 < dst_size; i++) {
        if (src[i] == '+') {
            dst[oi++] = ' ';
        } else if (src[i] == '%' && src[i + 1] != '\0' && src[i + 2] != '\0') {
            int hi = hex_val(src[i + 1]);
            int lo = hex_val(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[oi++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                dst[oi++] = src[i];
            }
        } else {
            dst[oi++] = src[i];
        }
    }
    dst[oi] = '\0';
}

/* 从表单 body 里取 key 的值（已 URL 解码） */
static bool form_get(const char *body, const char *key, char *out, size_t out_size)
{
    if (body == NULL || key == NULL || out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    char pattern[32];
    snprintf(pattern, sizeof(pattern), "%s=", key);

    const char *p = body;
    size_t plen = strlen(pattern);

    while (p != NULL && *p != '\0') {
        if (strncmp(p, pattern, plen) == 0) {
            const char *val = p + plen;
            const char *end = strchr(val, '&');
            size_t vlen = (end != NULL) ? (size_t)(end - val) : strlen(val);

            char tmp[300];
            if (vlen >= sizeof(tmp)) vlen = sizeof(tmp) - 1;
            memcpy(tmp, val, vlen);
            tmp[vlen] = '\0';

            url_decode(tmp, out, out_size);
            return true;
        }
        p = strchr(p, '&');
        if (p != NULL) p++;
    }
    return false;
}

/* UTF-8 文本 → percent-encoding（Bark 用） */
static void url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t oi = 0;
    for (size_t i = 0; src[i] != '\0' && oi + 4 < dst_size; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[oi++] = (char)c;
        } else {
            dst[oi++] = '%';
            dst[oi++] = hex[c >> 4];
            dst[oi++] = hex[c & 0x0F];
        }
    }
    dst[oi] = '\0';
}

/* ================================================================
 * WiFi 事件
 * ================================================================ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (base != WIFI_EVENT) {
        return;
    }

    switch (event_id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "STA 启动，开始连接...");
        esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "STA 断开 (reason=%d)", d ? d->reason : -1);

        if (s_state == APP_WIFI_AP_PROV) {
            break;
        }

        if (++s_retry <= STA_MAX_RETRY) {
            s_state = APP_WIFI_CONNECTING;
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "重试 %d 次仍失败，放弃连接", STA_MAX_RETRY);
            s_state = APP_WIFI_FAILED;
            if (s_evt) xEventGroupSetBits(s_evt, BIT_FAIL);
        }
        break;
    }

    case WIFI_EVENT_AP_START:
        ESP_LOGI(TAG, "配网热点已启动");
        s_state = APP_WIFI_AP_PROV;
        break;

    case WIFI_EVENT_AP_STOP:
        ESP_LOGI(TAG, "配网热点已关闭");
        break;

    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)arg;

    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "联网成功，IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        s_state = APP_WIFI_CONNECTED;
        if (s_evt) xEventGroupSetBits(s_evt, BIT_GOT_IP);
    }
}

/* ================================================================
 * 模式切换
 * ================================================================ */

static esp_err_t start_sta(const char *ssid)
{
    if (s_netif_sta == NULL) {
        s_netif_sta = esp_netif_create_default_wifi_sta();
        if (s_netif_sta == NULL) {
            ESP_LOGE(TAG, "创建 STA netif 失败");
            return ESP_FAIL;
        }
    }

    char pass[64] = { 0 };
    nvs_load_str(NVS_KEY_WIFI_PASS, pass, sizeof(pass));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = (pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) return ret;
    ret = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (ret != ESP_OK) return ret;

    s_state = APP_WIFI_CONNECTING;
    ret = esp_wifi_start();
    if (ret != ESP_OK) return ret;
    s_wifi_started = true;

    ESP_LOGI(TAG, "STA 模式，目标 SSID: %s", ssid);
    return ESP_OK;
}

static esp_err_t start_ap(void)
{
    if (s_netif_ap == NULL) {
        s_netif_ap = esp_netif_create_default_wifi_ap();
        if (s_netif_ap == NULL) {
            ESP_LOGE(TAG, "创建 AP netif 失败");
            return ESP_FAIL;
        }
    }

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), WIFI_AP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.ap.ssid, s_ap_ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len       = (uint8_t)strlen(s_ap_ssid);
    wc.ap.channel        = WIFI_AP_CHANNEL;
    wc.ap.max_connection = 2;

    if (strlen(WIFI_AP_PASSWORD) >= 8) {
        strlcpy((char *)wc.ap.password, WIFI_AP_PASSWORD, sizeof(wc.ap.password));
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wc.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) return ret;
    ret = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (ret != ESP_OK) return ret;

    s_state = APP_WIFI_AP_PROV;
    ret = esp_wifi_start();
    if (ret != ESP_OK) return ret;
    s_wifi_started = true;

    ESP_LOGW(TAG, "==================================================");
    ESP_LOGW(TAG, "  进入配网模式");
    ESP_LOGW(TAG, "  热点: %s   密码: %s", s_ap_ssid, WIFI_AP_PASSWORD);
    ESP_LOGW(TAG, "  手机连上后浏览器打开 http://192.168.4.1/");
    ESP_LOGW(TAG, "==================================================");
    return ESP_OK;
}

/* ================================================================
 * 公共接口
 * ================================================================ */

bool app_wifi_has_credentials(void)
{
    char ssid[33] = { 0 };
    if (nvs_load_str(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid)) != ESP_OK) {
        return false;
    }
    return ssid[0] != '\0';
}

esp_err_t app_wifi_init(void)
{
    if (s_evt == NULL) {
        s_evt = xEventGroupCreate();
        if (s_evt == NULL) return ESP_ERR_NO_MEM;
    }
    xEventGroupClearBits(s_evt, BIT_GOT_IP | BIT_FAIL);
    s_retry = 0;

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) return ret;

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    /* 先按 NVS 里的凭据决定创建 STA 还是 AP netif（标准顺序：
     * netif → wifi_init → set_mode → set_config → start） */
    char ssid[33] = { 0 };
    bool has_cred = (nvs_load_str(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid)) == ESP_OK)
                    && (ssid[0] != '\0');

    if (has_cred) {
        if (s_netif_sta == NULL) {
            s_netif_sta = esp_netif_create_default_wifi_sta();
        }
    } else {
        if (s_netif_ap == NULL) {
            s_netif_ap = esp_netif_create_default_wifi_ap();
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &ip_event_handler, NULL, NULL);

    if (has_cred) {
        return start_sta(ssid);
    }
    return start_ap();
}

esp_err_t app_wifi_connect(uint32_t timeout_ms)
{
    if (s_state == APP_WIFI_CONNECTED) {
        return ESP_OK;
    }
    if (s_state == APP_WIFI_AP_PROV || s_state == APP_WIFI_OFF) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_evt == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(s_evt, BIT_GOT_IP | BIT_FAIL,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    if (bits & BIT_GOT_IP) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

app_wifi_state_t app_wifi_state(void)
{
    return s_state;
}

const char *app_wifi_state_text(void)
{
    switch (s_state) {
    case APP_WIFI_AP_PROV:    return "配网中";
    case APP_WIFI_CONNECTING: return "连接中";
    case APP_WIFI_CONNECTED:  return "已联网";
    case APP_WIFI_FAILED:     return "未联网";
    case APP_WIFI_OFF:
    default:                  return "未联网";
    }
}

void app_wifi_set_sleep_callback(app_wifi_sleep_cb_t cb)
{
    s_sleep_cb = cb;
}

void app_wifi_stop(void)
{
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
        ESP_LOGI(TAG, "HTTP 服务已停止");
    }
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }
    s_state = APP_WIFI_OFF;
}

void app_wifi_kick_reconnect(void)
{
    if (!s_wifi_started) {
        return;
    }
    if (s_state == APP_WIFI_CONNECTED || s_state == APP_WIFI_AP_PROV) {
        return;
    }
    s_retry = 0;
    s_state = APP_WIFI_CONNECTING;
    esp_wifi_connect();
}

void app_wifi_clear_credentials(void)
{
    nvs_erase(NVS_KEY_WIFI_SSID);
    nvs_erase(NVS_KEY_WIFI_PASS);
    ESP_LOGW(TAG, "WiFi 凭据已清除，下次启动进入配网模式");
}

/* ================================================================
 * HTTP 页面
 * ================================================================ */

static const char PAGE_PROV_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>干湿衣架 · 配网</title><style>"
    "body{font-family:-apple-system,'Microsoft YaHei',sans-serif;margin:0;padding:18px;"
    "background:#f4f6f9;color:#222}"
    "h2{margin:0 0 2px;font-size:20px}p.sub{color:#8a8f98;font-size:13px;margin:0 0 14px}"
    "label{display:block;font-size:13px;color:#555;margin:14px 0 5px}"
    "input,select{width:100%;box-sizing:border-box;padding:11px;font-size:15px;"
    "border:1px solid #ccd2da;border-radius:9px;background:#fff}"
    "button{width:100%;margin-top:20px;padding:13px;font-size:16px;border:0;"
    "border-radius:9px;background:#2f6fed;color:#fff}"
    "</style></head><body>"
    "<h2>智能干湿衣架</h2>"
    "<p class='sub'>填写家里的 WiFi 和接收通知的地址</p>"
    "<form method='POST' action='/save'>"
    "<label>WiFi 名称 (SSID)</label>"
    "<input name='ssid' required maxlength='32' placeholder='家里路由器的名字'>"
    "<label>WiFi 密码</label>"
    "<input name='pass' type='password' maxlength='63' placeholder='留空表示开放网络'>"
    "<label>通知地址 (webhook)</label>"
    "<input name='url' maxlength='255' placeholder='https://...'>"
    "<label>通知格式</label><select name='fmt'>"
    "<option value='0'>通用 JSON（自建服务）</option>"
    "<option value='1'>企业微信 / 钉钉 机器人</option>"
    "<option value='2'>飞书 机器人</option>"
    "<option value='3'>Bark (iOS)</option>"
    "</select>"
    "<button type='submit'>保存并重启</button>"
    "</form></body></html>";

static esp_err_t handler_root(httpd_req_t *req)
{
    static char html[2048];
    env_reading_t r = { 0 };
    bool ok = app_core_get_last(&r);

    if (s_state == APP_WIFI_AP_PROV) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_send(req, PAGE_PROV_HEAD, HTTPD_RESP_USE_STRLEN);
    }

    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='10'>"
        "<title>干湿衣架</title><style>"
        "body{font-family:-apple-system,'Microsoft YaHei',sans-serif;margin:0;padding:18px;"
        "background:#f4f6f9;color:#222}"
        "h2{margin:0 0 14px;font-size:18px}"
        ".card{background:#fff;border-radius:14px;padding:18px;box-shadow:0 1px 6px #0001}"
        ".big{font-size:40px;font-weight:600;letter-spacing:-1px}"
        ".unit{font-size:18px;color:#8a8f98;margin-left:4px}"
        ".row{display:flex;gap:12px;margin-top:14px}"
        ".row div{flex:1;background:#fff;border-radius:14px;padding:14px;box-shadow:0 1px 6px #0001}"
        ".k{font-size:12px;color:#8a8f98}.v{font-size:20px;font-weight:600;margin-top:4px}"
        "</style></head><body>"
        "<h2>智能干湿衣架 <span style='float:right;font-size:13px;color:#2f6fed'>%s</span></h2>"
        "<div class='card'><div class='big'>%.1f<span class='unit'>°C</span> "
        "%.0f<span class='unit'>%%</span></div>"
        "<div class='k' style='margin-top:8px'>%s · 露点 %.1f°C · 绝对湿度 %.1f g/m3</div></div>"
        "<div class='row'>"
        "<div><div class='k'>干湿状态</div><div class='v'>%s</div></div>"
        "<div><div class='k'>联网</div><div class='v'>%s</div></div>"
        "</div>"
        "<p style='font-size:12px;color:#8a8f98;margin-top:16px'>"
        "www <a href='/api/status'>/api/status</a> · "
        "<a href='/api/sleep'>/api/sleep</a> 进入低功耗</p>"
        "</body></html>",
        device_id(),
        ok ? r.temperature : 0.0f,
        ok ? r.humidity : 0.0f,
        ok ? dry_level_text(r.level) : "无数据",
        ok ? r.dew_point : 0.0f,
        ok ? r.abs_humidity : 0.0f,
        ok ? dry_level_text(r.level) : "--",
        app_wifi_state_text());

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_status(httpd_req_t *req)
{
    static char json[512];
    env_reading_t r = { 0 };
    bool ok = app_core_get_last(&r);

    if (ok) {
        snprintf(json, sizeof(json),
            "{\"device\":\"dryrack\",\"id\":\"%s\",\"valid\":true,"
            "\"temp\":%.1f,\"humi\":%.0f,\"level\":\"%s\",\"status\":\"%s\","
            "\"dew\":%.1f,\"ah\":%.1f,\"cold\":%s,\"wifi\":\"%s\"}",
            device_id(), r.temperature, r.humidity,
            dry_level_code(r.level), dry_level_text(r.level),
            r.dew_point, r.abs_humidity,
            r.cold ? "true" : "false",
            app_wifi_state_text());
    } else {
        snprintf(json, sizeof(json),
            "{\"device\":\"dryrack\",\"id\":\"%s\",\"valid\":false,\"wifi\":\"%s\"}",
            device_id(), app_wifi_state_text());
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_sleep(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"result\":\"sleeping\"}");

    ESP_LOGW(TAG, "收到网页休眠指令");

    /* 这里只置标志，真正的休眠由 main 的上报任务执行：
     * 若在本 handler 里直接进深睡，会调用 httpd_stop() 等待自身返回而死锁。 */
    if (s_sleep_cb != NULL) {
        s_sleep_cb();
    }
    return ESP_OK;
}

static esp_err_t handler_save(httpd_req_t *req)
{
    char body[512];

    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_OK;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv fail");
            return ESP_OK;
        }
        received += r;
    }
    body[received] = '\0';

    char ssid[33] = { 0 }, pass[64] = { 0 }, url[256] = { 0 }, fmt_s[8] = { 0 };
    form_get(body, "ssid", ssid, sizeof(ssid));
    form_get(body, "pass", pass, sizeof(pass));
    form_get(body, "url", url, sizeof(url));
    form_get(body, "fmt", fmt_s, sizeof(fmt_s));

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_OK;
    }

    uint8_t fmt = (uint8_t)atoi(fmt_s);
    if (fmt > 3) fmt = 0;

    nvs_save_str(NVS_KEY_WIFI_SSID, ssid);
    nvs_save_str(NVS_KEY_WIFI_PASS, pass);
    nvs_save_str(NVS_KEY_WEBHOOK_URL, url);
    nvs_save_u8(NVS_KEY_WEBHOOK_FMT, fmt);

    ESP_LOGI(TAG, "配网信息已保存: ssid=%s fmt=%d url=%s", ssid, fmt, url);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "</head><body style=\"font-family:-apple-system,'Microsoft YaHei',sans-serif;"
        "padding:40px;text-align:center;background:#f4f6f9\">"
        "<h2>保存成功</h2><p style='color:#8a8f98'>设备正在重启并连接 WiFi…</p>"
        "</body></html>");

    vTaskDelay(pdMS_TO_TICKS(900));
    esp_restart();
    return ESP_OK;
}

esp_err_t app_wifi_start_http_server(void)
{
    if (s_httpd != NULL) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;

    esp_err_t ret = httpd_start(&s_httpd, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败: %s", esp_err_to_name(ret));
        s_httpd = NULL;
        return ret;
    }

    const httpd_uri_t uri_root   = { .uri = "/",           .method = HTTP_GET,  .handler = handler_root,   .user_ctx = NULL };
    const httpd_uri_t uri_status = { .uri = "/api/status", .method = HTTP_GET,  .handler = handler_status, .user_ctx = NULL };
    const httpd_uri_t uri_sleep  = { .uri = "/api/sleep",  .method = HTTP_GET,  .handler = handler_sleep,  .user_ctx = NULL };
    const httpd_uri_t uri_save   = { .uri = "/save",       .method = HTTP_POST, .handler = handler_save,   .user_ctx = NULL };

    httpd_register_uri_handler(s_httpd, &uri_root);
    httpd_register_uri_handler(s_httpd, &uri_status);
    httpd_register_uri_handler(s_httpd, &uri_sleep);
    httpd_register_uri_handler(s_httpd, &uri_save);

    ESP_LOGI(TAG, "HTTP 服务已启动");
    return ESP_OK;
}

/* ================================================================
 * webhook 上报
 * ================================================================ */

static void build_json(const env_reading_t *r, char *out, size_t out_size)
{
    snprintf(out, out_size,
        "{\"device\":\"dryrack\",\"id\":\"%s\","
        "\"temp\":%.1f,\"humi\":%.0f,"
        "\"level\":\"%s\",\"status\":\"%s\","
        "\"dew\":%.1f,\"ah\":%.1f,\"cold\":%s,"
        "\"uptime\":%u}",
        device_id(),
        r->temperature, r->humidity,
        dry_level_code(r->level), dry_level_text(r->level),
        r->dew_point, r->abs_humidity,
        r->cold ? "true" : "false",
        (unsigned)(esp_timer_get_time() / 1000000LL));
}

static void build_message(const env_reading_t *r, char *out, size_t out_size)
{
    snprintf(out, out_size,
        "【干湿衣架 %s】%s\n温度 %.1f°C，湿度 %.0f%%\n露点 %.1f°C，绝对湿度 %.1f g/m3%s",
        device_id(),
        dry_level_text(r->level),
        r->temperature, r->humidity,
        r->dew_point, r->abs_humidity,
        r->cold ? "\n⚠ 低温，衣物干得慢" : "");
}

static esp_err_t http_post_json(const char *url, const char *payload)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = WEBHOOK_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_post_field(cli, payload, (int)strlen(payload));

    esp_err_t ret = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);

    if (ret == ESP_OK && status >= 200 && status < 300) {
        ESP_LOGI(TAG, "webhook 推送成功 (HTTP %d)", status);
    } else {
        ESP_LOGW(TAG, "webhook 推送失败: %s (HTTP %d)",
                 esp_err_to_name(ret), status);
        if (ret == ESP_OK) ret = ESP_FAIL;
    }

    esp_http_client_cleanup(cli);
    return ret;
}

static esp_err_t http_get_simple(const char *url)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = WEBHOOK_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        return ESP_FAIL;
    }

    esp_err_t ret = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);

    if (ret == ESP_OK && status >= 200 && status < 300) {
        ESP_LOGI(TAG, "webhook 推送成功 (HTTP %d)", status);
    } else {
        ESP_LOGW(TAG, "webhook 推送失败: %s (HTTP %d)",
                 esp_err_to_name(ret), status);
        if (ret == ESP_OK) ret = ESP_FAIL;
    }

    esp_http_client_cleanup(cli);
    return ret;
}

esp_err_t app_wifi_report(const env_reading_t *reading)
{
    if (reading == NULL || !reading->valid) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != APP_WIFI_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    char url[256] = { 0 };
    esp_err_t ret = nvs_load_str(NVS_KEY_WEBHOOK_URL, url, sizeof(url));
    if ((ret != ESP_OK || url[0] == '\0') && CONFIG_DRYRACK_WEBHOOK_URL[0] != '\0') {
        strlcpy(url, CONFIG_DRYRACK_WEBHOOK_URL, sizeof(url));
    }
    if (url[0] == '\0') {
        ESP_LOGW(TAG, "未配置 webhook 地址，跳过上报");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t fmt = 0;
    nvs_load_u8(NVS_KEY_WEBHOOK_FMT, &fmt, 0);

    char payload[WEBHOOK_BODY_MAX];
    char msg[256];

    switch (fmt) {
    case WEBHOOK_FMT_TEXT:
        build_message(reading, msg, sizeof(msg));
        snprintf(payload, sizeof(payload),
                 "{\"msgtype\":\"text\",\"text\":{\"content\":\"%s\"}}", msg);
        return http_post_json(url, payload);

    case WEBHOOK_FMT_FEISHU:
        build_message(reading, msg, sizeof(msg));
        snprintf(payload, sizeof(payload),
                 "{\"msg_type\":\"text\",\"content\":{\"text\":\"%s\"}}", msg);
        return http_post_json(url, payload);

    case WEBHOOK_FMT_BARK: {
        build_message(reading, msg, sizeof(msg));
        char encoded[900];
        url_encode(msg, encoded, sizeof(encoded));
        char full[1200];
        int n = snprintf(full, sizeof(full), "%s/%s", url, encoded);
        if (n <= 0 || n >= (int)sizeof(full)) {
            return ESP_ERR_INVALID_SIZE;
        }
        return http_get_simple(full);
    }

    case WEBHOOK_FMT_JSON:
    default:
        build_json(reading, payload, sizeof(payload));
        return http_post_json(url, payload);
    }
}
