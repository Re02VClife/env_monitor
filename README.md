# 智能干湿提醒衣架 (Smart Dry-Rack)

基于 ESP32-S3 的晾衣架干湿提醒器：用 AHT30 采集温湿度，屏幕直接显示「干燥 / 适宜 / 偏潮 / 潮湿」，
联网后把数据推送到 webhook（企业微信 / 钉钉 / 飞书 / Bark / 自建服务），
并支持低功耗模式——关屏后每 30 分钟自动唤醒采集上报一次，按 BOOT 键随时唤醒。

> 本分支由原「环境监测仪」改造而来：**SGP30 气体传感器已从硬件上拆除**，CO₂ / TVOC 相关功能全部移除，只保留温湿度链路。

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1%20%7C%20v5.5-blue)
![MCU](https://img.shields.io/badge/MCU-ESP32--S3-green)
![Display](https://img.shields.io/badge/Display-ST7789V%20320%C3%97240-orange)

## 功能

| 功能 | 说明 |
|------|------|
| 干湿分级 | 按相对湿度分 4 档，配色显示，另给出露点、绝对湿度、低温提示 |
| 屏幕显示 | 320×240 横屏直驱帧缓冲，无 LVGL，实时刷新（1 秒） |
| WiFi 联网 | STA 连接路由器，定时推送温湿度到 webhook |
| 手机配网 | 首次开机自动开热点，手机浏览器填 WiFi 和通知地址，无需重新编译 |
| 本地网页 | 局域网内访问设备 IP 可直接看实时数据、下发休眠指令 |
| 低功耗 | 关屏 + 关闭 WiFi，每 30 分钟 RTC 定时唤醒采集上报，BOOT 键唤醒 |
| RGB 指示 | WS2812B 按干湿等级显示绿 / 黄 / 红 |

## 硬件

| 元件 | 型号 |
|------|------|
| MCU | ESP32-S3-WROOM-1-N16R8 (Octal PSRAM 8MB) |
| 显示屏 | ST7789V SPI 2.4" 320×240 |
| 温湿度传感器 | AHT30 (I²C 0x38) |
| RGB 灯 | WS2812B 5050 |
| ~~气体传感器~~ | ~~SGP30~~ **已拆除** |

### 引脚连接

板型在 `idf.py menuconfig` → **Dry Rack 干湿衣架** → **硬件板型** 里选择，默认「原 ESP32-S3-WROOM-1 板」。

| 功能 | 原板 | Freenove 板 |
|------|------|-------------|
| I²C SDA | IO6 | IO1 |
| I²C SCL | IO5 | IO2 |
| LCD SCK | IO21 | IO21 |
| LCD MOSI | IO47 | IO42 |
| LCD RST / DC / CS | IO45 / IO40 / IO41 | 同左 |
| WS2812B | IO48 | IO48 |
| 重启键 | IO38 | IO38 |
| 开关机键 | IO39 | IO39 |
| BOOT 键（唤醒源） | IO0 | IO0 |
| LCD 背光（可选） | 默认不控制，见下文 | |

> ⚠️ ESP32-S3 只有 **GPIO0 ~ GPIO21** 属于 RTC IO，能在深睡中作为唤醒源。
> 本项目用板载 **BOOT 键 (GPIO0)** 做唤醒，开关机键 (IO39) 只负责「进入低功耗」。

## 界面

```
┌────────────────────────────────────┐
│  干湿提醒                  已联网  │  标题 + 联网状态
├────────────────────────────────────┤
│   温度            湿度             │
│  25.5 °C          65 %             │
├────────────────────────────────────┤
│ ████████████░░░░       适宜        │  湿度条 + 干湿状态
├────────────────────────────────────┤
│ 露点 12.3°C                        │
│ 绝对湿度 9.8 g/m3     低温慢干     │
└────────────────────────────────────┘
```

### 干湿判定规则

按相对湿度分档（阈值可在 [`app_config.h`](main/app_config.h) 调整）：

| 湿度 | 状态 | 颜色 | 晾衣建议 |
|------|------|------|----------|
| ≤ 45 % | 干燥 | 青色 | 衣物易干 |
| 45 ~ 62 % | 适宜 | 绿色 | 正常晾晒 |
| 62 ~ 78 % | 偏潮 | 黄色 | 干得慢 |
| > 78 % | 潮湿 | 红色 | 不易干、易返潮 |

温度低于 12 °C 时额外提示「低温慢干」。同时计算并上报：

- **绝对湿度** `AH = 216.7 × e / (T + 273.15)`（g/m³），e 为水汽分压
- **露点** 用 Magnus 公式推导，判断是否容易结露返潮

## 编译

需要 ESP-IDF **v5.5 或 v6.1**（本仓库在 v6.1 上验证通过）。

```bash
. $IDF_PATH/export.sh      # Windows: . $env:IDF_PATH\export.ps1

idf.py set-target esp32s3
idf.py menuconfig          # 按需选板型 / 修改低功耗间隔
idf.py build
idf.py -p COM3 flash monitor
```

工程不依赖任何托管组件（已去掉 `lvgl`、`led_strip`），首次编译无需联网下载。

## 首次配网

1. 烧录后设备启动，若 NVS 里没有 WiFi 凭据，会自动开热点：
   - SSID：`DryRack-XXXX`（XXXX 为 MAC 后 4 位）
   - 密码：`dryrack123`
2. 手机连上该热点，浏览器打开 **http://192.168.4.1/**
3. 填写：
   - **WiFi 名称 / 密码**（2.4 GHz）
   - **通知地址**（webhook，见下节，可留空）
   - **通知格式**
4. 点「保存并重启」，设备重启后按 STA 模式连接路由器。

> 换了路由器或密码？**长按 BOOT 键 8 秒**会清除 WiFi 配置并重启，重新进入配网模式。

## webhook 通知格式

在配网页面选择格式，服务端会收到对应的请求：

### 0 · 通用 JSON（默认）

`POST` 到你的地址，`Content-Type: application/json`：

```json
{
  "device": "dryrack",
  "id": "A1B2C3",
  "temp": 25.5,
  "humi": 65,
  "level": "normal",
  "status": "适宜",
  "dew": 18.1,
  "ah": 14.2,
  "cold": false,
  "uptime": 3600
}
```

### 1 · 企业微信 / 钉钉 机器人

```json
{"msgtype":"text","text":{"content":"【干湿衣架 A1B2C3】适宜\n温度 25.5°C，湿度 65%\n露点 18.1°C，绝对湿度 14.2 g/m3"}}
```

### 2 · 飞书 机器人

```json
{"msg_type":"text","content":{"text":"【干湿衣架 A1B2C3】适宜..."}}
```

### 3 · Bark (iOS)

`GET {你填的地址}/{URL 编码后的消息}`，即把消息直接拼在路径后面。

> HTTPS 地址可用，固件内置了 CA 证书包（`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`）。

## 低功耗模式

### 进入方式

- **长按开关机键 (IO39) 2 秒**
- 局域网访问 `http://<设备IP>/api/sleep`
- 手机页面上的休眠链接

### 低功耗时做什么

1. 冻结采集任务，关闭屏幕显示（DISPOFF）并关闭背光（若配置了背光 GPIO）
2. 关闭 WS2812B、停止 WiFi 与 HTTP 服务
3. 配置两个唤醒源后进入 deep sleep：
   - **RTC 定时器**：`CONFIG_DRYRACK_SLEEP_INTERVAL_MIN` 分钟（默认 30）后唤醒
   - **BOOT 键 (GPIO0)**：按下立即唤醒

### 唤醒后的行为

| 唤醒原因 | 行为 |
|----------|------|
| RTC 定时器 | **静默周期**：不点屏，采 3 次取平均 → 连 WiFi → 推送 webhook → 继续睡 |
| BOOT 键 | **正常模式**：点亮屏幕，恢复实时显示与联网 |
| 复位 / 重新上电 | 正常模式 |

### 背光很重要

如果 LCD 模块的背光直接接在 3.3 V 上，深睡时屏幕虽然全黑但背光仍亮，实测会多耗十几 mA。
若背光接到了某个 GPIO，请在 menuconfig 里填写：

```
Dry Rack 干湿衣架 → LCD 背光控制 GPIO → 填你的 IO 号
```

默认 `-1` 表示不受控。

## 按键

| 按键 | 操作 | 功能 |
|------|------|------|
| IO38 | 短按 | 重启 |
| IO39 | 长按 2 秒 | 进入低功耗模式 |
| BOOT (IO0) | 长按 8 秒 | 清除 WiFi 配置并重启（重新配网） |
| BOOT (IO0) | 深睡时按下 | 唤醒并回到正常模式 |

## 目录结构

```
env_monitor/
├── main/
│   ├── main.c              # 入口：按唤醒原因分派「静默周期」/「正常模式」
│   ├── app_config.h        # 引脚、阈值、联网与低功耗参数
│   ├── app_core.c/h        # AHT30 采集 + 绝对湿度/露点/干湿分级
│   ├── app_display.c/h     # 320×240 干湿界面
│   ├── app_wifi.c/h        # SoftAP 配网 + STA + HTTP 服务 + webhook 上报
│   ├── app_power.c/h       # 低功耗、唤醒源、开关屏
│   └── Kconfig.projbuild   # menuconfig 菜单
├── components/
│   ├── aht30_driver/       # AHT30 I²C 驱动
│   ├── st7789_display/     # ST7789V SPI 驱动（帧缓冲直驱）
│   ├── ws2812_led/         # WS2812B 驱动（原生 RMT，无外部依赖）
│   ├── display_assets/     # 预渲染 4bpp 位图字体（自动生成）
│   └── nvs_storage/        # NVS 键值存储
├── generate_display_assets.py  # 位图素材生成脚本
├── sdkconfig.defaults          # Kconfig 默认值
├── partitions.csv              # 分区表
├── esp.md                      # 详细设计文档
└── sgp30_diagnosis.md          # 历史记录：SGP30 故障排查（已拆除）
```

## 重新生成显示素材

改了界面文字或字号后：

```bash
python generate_display_assets.py    # 需要 Pillow
idf.py build
```

## 已知限制

- 只支持 2.4 GHz WiFi（ESP32-S3 硬件限制）
- 没有 RTC 电池，深睡后时间戳依赖 `uptime`，消息时间以接收端为准
- 深睡期间 BOOT 键以外的按键无法唤醒（IO38 / IO39 不是 RTC IO）
- 若 LCD 背光不可控，低功耗收益会明显打折
