/*
 * ws2812.c — WS2812B LED 驱动（原生 RMT 实现）
 *
 * 不依赖 espressif/led_strip 组件：该组件 v2.5.x 未显式 include
 * esp_heap_caps.h，在 ESP-IDF v6.x 下编译失败。这里直接用 RMT copy
 * 编码器把每个 bit 编码成一个 symbol，时序完全可控。
 *
 * 时序（WS2812B 数据手册）:
 *   bit 0: 高 0.40us ±150ns, 低 0.85us ±150ns
 *   bit 1: 高 0.80us ±150ns, 低 0.45us ±150ns
 *   reset: 低电平 > 50us
 *
 * RMT 分辨率取 20MHz（1 tick = 50ns），对应:
 *   bit 0: duration0 = 8,  duration1 = 17
 *   bit 1: duration0 = 16, duration1 = 9
 */
#include "ws2812.h"

#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_types.h"

static const char *TAG = "ws2812";

/* 硬件引脚（与 main/app_config.h 中的 WS2812_GPIO 保持一致） */
#define WS2812_GPIO_PIN     GPIO_NUM_48
#define WS2812_RESOLUTION_HZ (20 * 1000 * 1000)
#define WS2812_RESET_MS     1

/* 20MHz 下的一个 bit 的高/低电平 tick 数 */
#define T0H_TICKS   8
#define T0L_TICKS   17
#define T1H_TICKS   16
#define T1L_TICKS   9

static rmt_channel_handle_t s_chan    = NULL;
static rmt_encoder_handle_t s_encoder = NULL;

/* 单灯珠 24bit，复用静态缓冲避免异步发送时的悬垂指针 */
static rmt_symbol_word_t s_symbols[24];

/* ----------------------------------------------------------------
 * 内部工具
 * ---------------------------------------------------------------- */

static inline void encode_bit(rmt_symbol_word_t *sym, bool one)
{
    sym->level0    = 1;
    sym->duration0 = one ? T1H_TICKS : T0H_TICKS;
    sym->level1    = 0;
    sym->duration1 = one ? T1L_TICKS : T0L_TICKS;
}

/* ----------------------------------------------------------------
 * 公共接口
 * ---------------------------------------------------------------- */

esp_err_t ws2812_init(void)
{
    if (s_chan != NULL) {
        return ESP_OK;
    }

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num          = WS2812_GPIO_PIN,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = WS2812_RESOLUTION_HZ,
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
        .flags.with_dma    = false,
    };

    esp_err_t ret = rmt_new_tx_channel(&tx_cfg, &s_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RMT 通道创建失败: %s", esp_err_to_name(ret));
        s_chan = NULL;
        return ret;
    }

    /* rmt_copy_encoder_config_t 是空结构体，这里用空初始化 */
    rmt_copy_encoder_config_t enc_cfg = {};
    ret = rmt_new_copy_encoder(&enc_cfg, &s_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RMT 编码器创建失败: %s", esp_err_to_name(ret));
        rmt_del_channel(s_chan);
        s_chan = NULL;
        s_encoder = NULL;
        return ret;
    }

    ret = rmt_enable(s_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RMT 使能失败: %s", esp_err_to_name(ret));
        rmt_del_encoder(s_encoder);
        rmt_del_channel(s_chan);
        s_chan = NULL;
        s_encoder = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "WS2812B 初始化完成 (GPIO %d, 原生 RMT)", WS2812_GPIO_PIN);
    return ESP_OK;
}

esp_err_t ws2812_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_chan == NULL || s_encoder == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* WS2812B 的数据顺序是 GRB */
    const uint8_t grb[3] = { g, r, b };

    int idx = 0;
    for (int i = 0; i < 3; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            encode_bit(&s_symbols[idx++], ((grb[i] >> bit) & 0x01) != 0);
        }
    }

    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    esp_err_t ret = rmt_transmit(s_chan, s_encoder, s_symbols,
                                 sizeof(s_symbols), &tx_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = rmt_tx_wait_all_done(s_chan, 100);
    if (ret != ESP_OK) {
        return ret;
    }

    /* reset：数据线保持低电平 > 50us，灯珠才会锁存 */
    vTaskDelay(pdMS_TO_TICKS(WS2812_RESET_MS));
    return ESP_OK;
}

esp_err_t ws2812_set_color(led_color_t color)
{
    switch (color) {
    case LED_COLOR_GREEN:  return ws2812_set_rgb(0, 50, 0);
    case LED_COLOR_YELLOW: return ws2812_set_rgb(50, 35, 0);
    case LED_COLOR_RED:    return ws2812_set_rgb(50, 0, 0);
    case LED_COLOR_BLUE:   return ws2812_set_rgb(0, 0, 50);
    case LED_COLOR_OFF:
    default:               return ws2812_set_rgb(0, 0, 0);
    }
}
