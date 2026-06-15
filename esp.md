# 环境监测仪 — 项目文档

> 最后更新: 2026-06-15 — 显示重构：去除 LVGL，改为直驱帧缓冲横屏仪表盘

---

## 一、硬件规格

### 元件清单 (BOM)

| 位号 | 型号/名称 | 描述 |
|-|-|-|
| **U3** | ESP32-S3-WROOM-1-N16R8 | 主控MCU，Wi-Fi+蓝牙模组，Octal PSRAM 8MB |
| **U9** | 5V升压板 | 电源模块，电池升压至5V |
| **U4** | WAFER-PH2.0-2PZZ | 2Pin 2.0mm 电池/电源连接器 |
| **U7** | ST7789**V** | LCD显示屏（SPI接口）**注意是V版本** |
| **U1** | SGP30 | 气体传感器（CO₂/TVOC）I²C接口 |
| **U2** | AHT30 | 温湿度传感器，I²C接口 |
| **RGB1** | XL-5050RGBC-WS2812B | 5050幻彩LED，内置WS2812B驱动 |

### 引脚映射

> 切换板子时修改 [`app_config.h`](main/app_config.h) 和 [`st7789.c`](components/st7789_display/st7789.c) 顶部的宏定义。

#### 当前板：Freenove ESP32-S3 WROOM（单 I²C 总线）

| 功能 | 引脚 | 说明 |
|------|------|------|
| I²C 总线 | SDA=**IO1**, SCL=**IO2** | AHT30(0x38) + SGP30(0x58) 共用 |
| ST7789V SPI | SCK=IO21, SDA=**IO42**, RST=IO45, DC=IO40, CS=IO41 | IO42 避开 PSRAM IO47 |
| WS2812B | IO48 | ⚠️ Octal PSRAM 共用，可能不稳定 |
| UART 调试 | TX=IO37, RX=IO36 | |

#### 旧板：原 ESP32-S3-WROOM-1 开发板（双 I²C 总线）

| 功能 | 引脚 | 说明 |
|------|------|------|
| AHT30 I2C | SDA=**IO6**, SCL=**IO5** | |
| SGP30 I2C | SDA=IO20, SCL=IO19 | |
| ST7789V SPI | SCK=IO21, SDA=**IO47**, RST=IO45, DC=IO40, CS=IO41 | |
| WS2812B | IO48 | |
| UART 调试 | TX=IO37, RX=IO36 | |

#### Freenove 板注意事项

| 避免使用 | 原因 |
|----------|------|
| GPIO 47 | Octal PSRAM DQS 信号，不可用 |
| GPIO 48 | Octal PSRAM CLK_N，不可靠 |
| GPIO 46 | VDD_SPI Strapping 脚 |
| GPIO 5, 6 | CAMA 摄像头 I²C 接口，有外围电路干扰 |
| GPIO 43, 44 | UART0 控制台 (TX/RX) |

### 电源网络

| 网络 | 连接节点 | 电压 |
|------|---------|------|
| **GND** | U3(1,40,41), U9(1,3), U4(1), U7(1) | 0V |
| **3V3** | U3(2), U7(2), U1(1), U2(4) | 3.29V ✅ |
| **5V** | U9(2,4), U4(2), RGB1(1) | 5V |

---

## 二、软件状态

### 模块状态总览

| 模块 | 状态 | 备注 |
|------|------|------|
| AHT30 温湿度 | ✅ 工作 | 每秒采集，数据正常 |
| ST7789V 显示屏 | ✅ 工作 | **横屏 320×240**，直接帧缓冲渲染 |
| WS2812B LED | ✅ 工作 | CO2浓度颜色指示 |
| 仪表盘界面 | ✅ 工作 | 简单直驱，4bpp 位图字体，无 LVGL |
| NVS 存储 | ✅ 工作 | SGP30基线持久化（待SGP30修复后使用） |
| SGP30 气体传感器 | ✅ 工作 | I2C Probe 成功，CO2=400ppm 基线稳定中 |

### 横屏仪表盘布局 (320×240)

```
┌────────────────────────────────────┐   0
│           环境监测仪               │   标题栏 34px
├────────────────────────────────────┤
│                                    │
│        CO2   888   ppm             │   CO2 大数字 (48px)
│                                    │
├─────────────────┬──────────────────┤
│  温度  25.5°C   │   湿度  65%      │   温湿度 (32px 数字)
├─────────────────┴──────────────────┤
│ ████████░░░░░░░░  良好             │   CO2 进度条 + 状态
│ TVOC: xxx ppb                      │   底部信息
└────────────────────────────────────┘
```

### 技术架构

- **显示渲染**: 直接操作 RGB565 帧缓冲 (153.6KB)，`app_display_update()` 每帧局部擦除+重绘后调用 `st7789_flush()` 全屏刷新
- **字体渲染**: 预渲染 4bpp 灰度位图 + alpha 混合，无乱码风险。素材由 `generate_display_assets.py` 生成
- **无 LVGL 依赖**: 编译更快、固件更小、无内存池开销

### 已解决的问题

#### 1. I2C 总线分配混乱（已修复）
- **原因**: `app_config.h` 注释和 `app_core.c` 的日志/代码互相矛盾
- **修复**: 统一按硬件连接：I2C0→AHT30(IO5/6), I2C1→SGP30(IO19/20)
- **文件**: [`app_config.h`](main/app_config.h), [`app_core.c`](main/app_core.c)

#### 2. GPIO19/20 被 USB Serial/JTAG 占用（已修复）
- **原因**: `CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=y` 导致 USB 控制器占用这两个引脚
- **修复**: `sdkconfig.defaults` 添加 `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`
- **文件**: [`sdkconfig.defaults`](sdkconfig.defaults)

#### 3. AHT30 虚假 CRC 警告（已修复）
- **原因**: 6字节响应格式不含CRC字节
- **修复**: 移除无效CRC校验，删除 `aht30_crc8` 函数
- **文件**: [`aht30.c`](components/aht30_driver/aht30.c)

#### 4. ST7789V 背光亮但黑屏（已修复）
- **原因**: ST7789**V** 需要额外电压/Gamma配置
- **修复**: 初始化序列添加 VCOM/VRH/VDV/Gamma 命令
- **文件**: [`st7789.c`](components/st7789_display/st7789.c)

#### 5. 中文字体乱码 — 去除 LVGL 直驱方案（最终解决）
- **原因**: LVGL v9 字体系统复杂，18px 2bpp 字体抗锯齿差、看不清
- **修复**: 完全移除 LVGL，改用 `generate_display_assets.py` 预渲染 24px/32px/48px 4bpp 位图字体，alpha 混合直接绘制到帧缓冲
- **文件**: [`generate_display_assets.py`](generate_display_assets.py), [`display_assets/`](components/display_assets/)

#### 6. 代码健壮性修复
- `sgp30_init()` 探针失败立即返回错误
- SGP30 不可用时跳过预热阶段
- ws2812.c 统一使用 `app_config.h` 阈值
- st7789.c 移除 3秒调试残留

---

## 三、SGP30 故障排查记录

### 诊断过程

1. 初始状态：SGP30 在 I2C0(IO5/IO6) 失败 → `ESP_ERR_INVALID_STATE`
2. 交换总线：SGP30 在 I2C1(IO19/IO20) 仍失败
3. AHT30 在两个总线上都正常 → 排除 I2C 控制器和引脚问题
4. USB 占用排除：禁用 USB Serial/JTAG 后 SGP30 仍失败
5. **`i2c_master_probe(0x58)` 返回 `ESP_ERR_NOT_FOUND`** → 芯片完全不应答

### 结论

**SGP30 芯片在 I2C 总线上完全不应答其地址 (0x58)，属于硬件层面的问题。**

---

## 四、文件结构

```
env_monitor/
├── main/
│   ├── main.c              # 入口：FreeRTOS任务创建（无LVGL）
│   ├── app_core.c/h        # 传感器采集核心（I2C总线+状态机）
│   ├── app_display.c/h     # 简易仪表盘（直接帧缓冲绘制）
│   ├── app_config.h        # 引脚/I2C/阈值集中配置
│   └── CMakeLists.txt
├── components/
│   ├── aht30_driver/       # AHT30 温湿度 I2C 驱动
│   ├── sgp30_driver/       # SGP30 气体传感器 I2C 驱动（含 probe 诊断）
│   ├── st7789_display/     # ST7789V SPI 驱动（横屏，帧缓冲，无 LVGL）
│   ├── ws2812_led/         # WS2812B RGB LED 驱动
│   ├── nvs_storage/        # NVS 基线存储
│   └── display_assets/     # 预渲染位图素材（4bpp 灰度，自动生成）
├── generate_display_assets.py  # 显示素材生成脚本
├── generate_font.py            # （已弃用）LVGL 字体生成脚本
├── sdkconfig.defaults          # Kconfig 默认值
├── partitions.csv              # 分区表
└── esp.md                      # 本文档
```

---

## 五、常用操作

### 切换板子配置

修改两个文件后重新编译：

**1.** [`app_config.h`](main/app_config.h) — I²C 引脚：
```c
// Freenove 板:
#define I2C_SDA_IO   GPIO_NUM_1
#define I2C_SCL_IO   GPIO_NUM_2
// 原板 (双总线):
#define I2C_SDA_IO   GPIO_NUM_6
#define I2C_SCL_IO   GPIO_NUM_5
```

**2.** [`st7789.c`](components/st7789_display/st7789.c) — 显示屏 MOSI：
```c
// Freenove 板:
#define LCD_MOSI_IO  GPIO_NUM_42
// 原板:
#define LCD_MOSI_IO  GPIO_NUM_47
```

### 修改显示文字或字号
编辑 [`generate_display_assets.py`](generate_display_assets.py) 中的 `ASSETS` 列表，然后：
```bash
python generate_display_assets.py
```
重新编译烧录即可，无需修改 C 代码。注意需要使用 ESP-IDF 自带的 Python（已含 Pillow）：
```bash
E:/esp/Espressif/python_env/idf5.5_py3.11_env/Scripts/python.exe generate_display_assets.py
```

### 修改 I2C 引脚
编辑 [`app_config.h`](main/app_config.h) 中的 `I2C0_SCL_IO` / `I2C0_SDA_IO` / `I2C1_SCL_IO` / `I2C1_SDA_IO` 宏。

### 修改 LCD 引脚
编辑 [`st7789.c`](components/st7789_display/st7789.c) 顶部的 `LCD_SCLK_IO` / `LCD_MOSI_IO` / `LCD_CS_IO` / `LCD_DC_IO` / `LCD_RST_IO` 宏。

### 修改 WS2812B LED 引脚
编辑 [`ws2812.c`](components/ws2812_led/ws2812.c) 顶部的 `WS2812_GPIO` / `WS2812_NUM_LEDS` 宏。

### 修改 CO2 阈值
编辑 [`app_config.h`](main/app_config.h) 中的 `CO2_WARN_THRESHOLD` / `CO2_ALERT_THRESHOLD` 宏（同时影响仪表盘颜色和 LED 指示）。
