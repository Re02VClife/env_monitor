/*
 * st7789.c — ST7789 LCD 驱动（无 LVGL）
 * 横屏 320×240, SPI2, RGB565, 帧缓冲直接操作
 */
#include "st7789.h"

#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "sdkconfig.h"

/* ---- 硬件引脚 ---- */
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_SCLK_IO     GPIO_NUM_21
#define LCD_CS_IO       GPIO_NUM_41
#define LCD_DC_IO       GPIO_NUM_40
#define LCD_RST_IO      GPIO_NUM_45

/* MOSI 随板型变化：Octal PSRAM 板不能用 IO47 */
#if CONFIG_DRYRACK_BOARD_FREENOVE
#define LCD_MOSI_IO     GPIO_NUM_42
#else
#define LCD_MOSI_IO     GPIO_NUM_47
#endif

/* 背光控制（可选，-1 表示背光直接接电源） */
#if CONFIG_DRYRACK_LCD_BL_GPIO >= 0
#define LCD_BL_IO       ((gpio_num_t)CONFIG_DRYRACK_LCD_BL_GPIO)
#define HAS_BACKLIGHT   1
#else
#define HAS_BACKLIGHT   0
#endif

/* ---- 横屏分辨率 ---- */
#define LCD_W   320
#define LCD_H   240

/* ---- SPI 时钟 ---- */
#define LCD_PCLK_HZ  (20 * 1000 * 1000)

static const char *TAG = "st7789";

static esp_lcd_panel_io_handle_t io_handle  = NULL;
static esp_lcd_panel_handle_t    panel      = NULL;
static uint16_t *framebuffer = NULL;
static bool s_display_on = true;

/* ================================================================
 * 背光
 * ================================================================ */

static void backlight_init(void)
{
#if HAS_BACKLIGHT
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << LCD_BL_IO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);
    gpio_set_level(LCD_BL_IO, 1);
    ESP_LOGI(TAG, "背光控制 GPIO%d 已启用", LCD_BL_IO);
#endif
}

bool st7789_has_backlight(void)
{
#if HAS_BACKLIGHT
    return true;
#else
    return false;
#endif
}

/* ================================================================
 * 公开接口
 * ================================================================ */

esp_err_t st7789_init(void)
{
    if (framebuffer != NULL) {
        ESP_LOGW(TAG, "ST7789 已初始化，跳过");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "初始化 ST7789 横屏 %d×%d (MOSI=IO%d)...",
             LCD_W, LCD_H, LCD_MOSI_IO);

    /* 1. SPI 总线 */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num     = LCD_SCLK_IO,
        .mosi_io_num     = LCD_MOSI_IO,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_W * LCD_H * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* 2. 面板 IO */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = LCD_CS_IO,
        .dc_gpio_num       = LCD_DC_IO,
        .spi_mode          = 0,
        .pclk_hz           = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io_handle));

    /* 3. 面板设备（IDF v5.2+ 用 rgb_ele_order 取代已移除的 color_space） */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST_IO,
        .bits_per_pixel = 16,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel));

    /* 4. 初始化序列 */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    /* 4a. ST7789V 电压和 Gamma */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xB2, (uint8_t[]){0x0C,0x0C,0x00,0x33,0x33}, 5));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xB7, (uint8_t[]){0x35}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xBB, (uint8_t[]){0x20}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xC0, (uint8_t[]){0x2C}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xC2, (uint8_t[]){0x01}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xC3, (uint8_t[]){0x12}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xC4, (uint8_t[]){0x20}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xC6, (uint8_t[]){0x0F}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xD0, (uint8_t[]){0xA4,0xA1}, 2));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xE0, (uint8_t[]){
        0xD0,0x04,0x0D,0x11,0x13,0x2B,0x3F,0x54,0x4C,0x18,0x0D,0x0B,0x1F,0x23
    }, 14));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xE1, (uint8_t[]){
        0xD0,0x04,0x0C,0x11,0x13,0x2C,0x3F,0x44,0x51,0x2F,0x1F,0x1F,0x20,0x23
    }, 14));

    /* 颜色反转 */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));

    /* 横屏: 交换 XY 并镜像 */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));

    /* 消除间隙 */
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 0, 0));

    /* 开启显示 */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    /* 4b. 背光（如果硬件支持） */
    backlight_init();

    /* 5. 分配帧缓冲 (320×240×2 = 153.6 KB, 放内部 DRAM) */
    framebuffer = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (framebuffer == NULL) {
        ESP_LOGE(TAG, "帧缓冲分配失败!");
        return ESP_ERR_NO_MEM;
    }

    /* 6. 清屏黑色 */
    memset(framebuffer, 0x00, LCD_W * LCD_H * sizeof(uint16_t));
    esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, framebuffer);

    ESP_LOGI(TAG, "ST7789 初始化成功 (%d×%d 横屏, FB=%dKB, 背光%s)",
             LCD_W, LCD_H, (int)(LCD_W * LCD_H * 2 / 1024),
             st7789_has_backlight() ? "可控" : "常亮");
    return ESP_OK;
}

uint16_t *st7789_get_fb(void)
{
    return framebuffer;
}

void st7789_flush(void)
{
    if (framebuffer && panel) {
        esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, framebuffer);
    }
}

void st7789_display_off(void)
{
    if (panel == NULL) {
        return;
    }

    /* 先刷成纯黑，再发 DISPOFF，避免残留亮像素 */
    if (framebuffer) {
        memset(framebuffer, 0x00, LCD_W * LCD_H * sizeof(uint16_t));
        esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, framebuffer);
    }
    esp_lcd_panel_disp_on_off(panel, false);

#if HAS_BACKLIGHT
    gpio_set_level(LCD_BL_IO, 0);
#endif

    s_display_on = false;
    ESP_LOGI(TAG, "显示已关闭");
}

void st7789_display_on(void)
{
    if (panel == NULL || s_display_on) {
        return;
    }

#if HAS_BACKLIGHT
    gpio_set_level(LCD_BL_IO, 1);
#endif
    esp_lcd_panel_disp_on_off(panel, true);
    s_display_on = true;
    ESP_LOGI(TAG, "显示已开启");
}

int st7789_width(void)  { return LCD_W; }
int st7789_height(void) { return LCD_H; }
