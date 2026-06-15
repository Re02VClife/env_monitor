# 环境监测仪 (Environment Monitor)

基于 ESP32-S3 的桌面环境监测仪，实时显示温度、湿度、CO₂ 浓度和 TVOC。

![仪表盘](https://img.shields.io/badge/ESP--IDF-v5.5.3-blue) ![MCU](https://img.shields.io/badge/MCU-ESP32--S3-green) ![Display](https://img.shields.io/badge/Display-ST7789V%20320%C3%97240-orange)

## 硬件

| 元件 | 型号 |
|------|------|
| MCU | ESP32-S3-WROOM-1-N16R8 (Octal PSRAM 8MB) |
| 显示屏 | ST7789V SPI 2.4" 320×240 |
| 温湿度传感器 | AHT30 (I²C 0x38) |
| 气体传感器 | SGP30 (I²C 0x58, CO₂ + TVOC) |
| LED | WS2812B 5050 幻彩 |

### 引脚连接

| 功能 | 引脚 |
|------|------|
| I²C (AHT30+SGP30 共用) | SDA=IO1, SCL=IO2 |
| LCD SPI | SCK=IO21, SDA=IO42, RST=IO45, DC=IO40, CS=IO41 |
| WS2812B | IO48 |
| 按键 (重启) | IO38 |
| 按键 (开关机) | IO39 |

## 仪表盘

横屏 320×240 直接帧缓冲渲染，无 LVGL 依赖，黑底白字：

```
 CO2               888              ppm
 ─────────────────────────────────────
    温度                湿度
  25.5 °C              65 %
 ─────────────────────────────────────
 ████████░░░░░░░░  良好
```

## 编译

```bash
# 安装 ESP-IDF v5.5+
# 克隆仓库
git clone https://github.com/yourname/env_monitor.git
cd env_monitor

# 编译烧录
idf.py build
idf.py flash
idf.py monitor
```

## 自定义

```bash
# 重新生成显示字体素材
python generate_display_assets.py

# 修改引脚: components/st7789_display/st7789.c
# 修改 I²C: main/app_config.h
```

## 文件结构

```
env_monitor/
├── main/
│   ├── main.c              # 入口
│   ├── app_core.c/h        # 传感器采集
│   ├── app_display.c/h     # 仪表盘直接帧缓冲渲染
│   └── app_config.h        # 集中配置
├── components/
│   ├── st7789_display/     # ST7789V SPI 驱动
│   ├── display_assets/     # 预渲染位图字体
│   ├── aht30_driver/       # AHT30 I²C 驱动
│   ├── sgp30_driver/       # SGP30 I²C 驱动
│   ├── ws2812_led/         # WS2812B LED 驱动
│   └── nvs_storage/        # NVS 基线存储
├── generate_display_assets.py  # 素材生成脚本
├── sdkconfig.defaults          # Kconfig 默认值
└── esp.md                      # 详细项目文档
```
