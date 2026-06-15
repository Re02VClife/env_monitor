/*
 * ws2812.c — WS2812B LED 驱动实现
 * 使用 espressif/led_strip 组件 (RMT 后端)
 */
#include "ws2812.h"
#include "led_strip.h"
#include "esp_log.h"
#include "hal/gpio_types.h"
#include "app_config.h"    /* 使用集中配置中的 CO2 阈值 */

static const char *TAG = "ws2812";

/* 硬件引脚定义（组件内部，不通过 app_config.h 暴露） */
#define WS2812_GPIO       GPIO_NUM_48
#define WS2812_NUM_LEDS   1

static led_strip_handle_t led_handle = NULL;

/* ----------------------------------------------------------------
 * 公共接口
 * ---------------------------------------------------------------- */

esp_err_t ws2812_init(void)
{
    if (led_handle != NULL) {
        return ESP_OK;
    }

    /* LED 配置 */
    led_strip_config_t strip_cfg = {
        .strip_gpio_num   = WS2812_GPIO,
        .max_leds         = WS2812_NUM_LEDS,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model        = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    /* RMT 后端配置 */
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  /* 10 MHz */
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &led_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 点亮蓝色表示初始化中 */
    ws2812_set_color(LED_COLOR_BLUE);
    ESP_LOGI(TAG, "WS2812B LED 初始化完成 (GPIO %d)", WS2812_GPIO);
    return ESP_OK;
}

esp_err_t ws2812_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (led_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = led_strip_set_pixel(led_handle, 0, r, g, b);
    if (ret != ESP_OK) return ret;
    return led_strip_refresh(led_handle);
}

esp_err_t ws2812_set_co2_level(uint16_t co2_ppm)
{
    if (co2_ppm < CO2_WARN_THRESHOLD) {
        return ws2812_set_color(LED_COLOR_GREEN);
    } else if (co2_ppm < CO2_ALERT_THRESHOLD) {
        return ws2812_set_color(LED_COLOR_YELLOW);
    } else {
        return ws2812_set_color(LED_COLOR_RED);
    }
}

esp_err_t ws2812_set_color(led_color_t color)
{
    switch (color) {
    case LED_COLOR_GREEN:
        return ws2812_set_rgb(0, 60, 0);
    case LED_COLOR_YELLOW:
        return ws2812_set_rgb(60, 40, 0);
    case LED_COLOR_RED:
        return ws2812_set_rgb(60, 0, 0);
    case LED_COLOR_BLUE:
        return ws2812_set_rgb(0, 0, 60);
    case LED_COLOR_OFF:
    default:
        return ws2812_set_rgb(0, 0, 0);
    }
}
