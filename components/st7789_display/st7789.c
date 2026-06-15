/*
 * st7789.c — ST7789 LCD 驱动（简化版）
 * 横屏 320×240, SPI2, RGB565, 无 LVGL 依赖
 */
#include "st7789.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

/* ---- 硬件引脚 ---- */
#define LCD_SPI_HOST        SPI2_HOST
#define LCD_SCLK_IO         GPIO_NUM_21
#define LCD_MOSI_IO         GPIO_NUM_42   /* 避开 GPIO47(PSRAM DQS) 和 GPIO46(Strapping) */
#define LCD_CS_IO           GPIO_NUM_41
#define LCD_DC_IO           GPIO_NUM_40
#define LCD_RST_IO          GPIO_NUM_45

/* ---- 横屏分辨率 ---- */
#define LCD_W   320
#define LCD_H   240

/* ---- SPI 时钟 ---- */
#define LCD_PCLK_HZ  (20 * 1000 * 1000)  /* 20MHz 更稳定，兼容不同板子 */

static const char *TAG = "st7789";

static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t   panel     = NULL;
static uint16_t *framebuffer = NULL;

/* ================================================================
 * 公开接口
 * ================================================================ */

esp_err_t st7789_init(void)
{
    if (framebuffer != NULL) {
        ESP_LOGW(TAG, "ST7789 已初始化，跳过");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "初始化 ST7789 横屏 %d×%d...", LCD_W, LCD_H);

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

    /* 3. 面板设备 */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST_IO,
        .bits_per_pixel = 16,
        .color_space    = ESP_LCD_COLOR_SPACE_BGR,
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

    /* 5. 分配帧缓冲 (320×240×2 = 153.6 KB, 放 DRAM) */
    framebuffer = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (framebuffer == NULL) {
        ESP_LOGE(TAG, "帧缓冲分配失败!");
        return ESP_ERR_NO_MEM;
    }

    /* 6. 清屏黑色 */
    memset(framebuffer, 0x00, LCD_W * LCD_H * sizeof(uint16_t));
    esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, framebuffer);

    ESP_LOGI(TAG, "ST7789 初始化成功 (%d×%d 横屏, FB=%dKB)",
             LCD_W, LCD_H, (int)(LCD_W * LCD_H * 2 / 1024));
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

int st7789_width(void)  { return LCD_W; }
int st7789_height(void) { return LCD_H; }
