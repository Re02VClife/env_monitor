# 智能干湿提醒衣架 — 设计文档

> 最后更新: 2026-11-08 — 由「环境监测仪」改造：拆除 SGP30，新增 WiFi 上报、手机配网、低功耗周期采集

---

## 一、硬件

### BOM

| 位号 | 型号 | 说明 |
|------|------|------|
| U3 | ESP32-S3-WROOM-1-N16R8 | 主控，Wi-Fi + BLE，Octal PSRAM 8MB |
| U9 | 5V 升压板 | 电池升压 |
| U4 | WAFER-PH2.0-2PZZ | 电池/电源连接器 |
| U7 | ST7789**V** | LCD 显示屏（SPI，注意是 V 版本） |
| U2 | AHT30 | 温湿度传感器（I²C 0x38） |
| RGB1 | XL-5050RGBC-WS2812B | 幻彩 LED |
| ~~U1~~ | ~~SGP30~~ | **已拆除**，原 CO₂/TVOC 传感器 |

### 引脚

> 完整原理图、PCB 与器件实物图见 [README · 硬件资料](README.md#硬件资料)，原图归档在 [`docs/hardware/`](docs/hardware/)。

板型二选一，在 menuconfig 的 `Dry Rack 干湿衣架 → 硬件板型` 中切换；
引脚常量分别位于 [`app_config.h`](main/app_config.h)（I²C）和 [`st7789.c`](components/st7789_display/st7789.c)（LCD）。

| 功能 | 本板（原理图，默认） | Freenove 板 |
|------|-------------|-------------|
| I²C SDA | IO6 | IO1 |
| I²C SCL | IO5 | IO2 |
| LCD SCK | IO21 | IO21 |
| LCD MOSI | IO47 | IO42 |
| LCD RST | IO45 | IO45 |
| LCD DC | IO40 | IO40 |
| LCD CS | IO41 | IO41 |
| LCD 背光 BLK | **悬空（背光常亮）**，可用 `CONFIG_DRYRACK_LCD_BL_GPIO` 指定，默认 -1 | — |
| WS2812B DIN | IO48（VDD 接 5 V） | IO48 |
| 重启键 | IO38 | IO38 |
| 开关机键 | IO39 | IO39 |
| BOOT / 唤醒键 | IO0（R2 10 K 上拉 + C5 100 nF） | IO0 |
| 电池电压检测 | IO2（R3/R4 各 100 K 分压 + C6 100 nF） | — |

**本板的两条硬约束**：

1. **BLK 悬空**：`DISPOFF` 只能让屏幕变黑，背光仍亮，低功耗收益打折。
   要真正省电需飞线把 BLK 接到空闲 IO（IO19 / IO20），再在 menuconfig 里配置背光 GPIO。
2. **IO2 已被电量检测占用**：所以本板不能选 Freenove 板型（会与分压电路冲突）；
   该电压目前固件未使用，属硬件预留。

**Freenove 板避坑**：

| 避免使用 | 原因 |
|----------|------|
| GPIO 47 | Octal PSRAM DQS |
| GPIO 48 | Octal PSRAM CLK_N（WS2812 仍沿用，可能不稳定） |
| GPIO 46 | VDD_SPI strapping |
| GPIO 5 / 6 | CAMA 摄像头 I²C 外围电路干扰 |
| GPIO 43 / 44 | UART0 |

### 电源

| 网络 | 节点 | 电压 |
|------|------|------|
| GND | U3(1,40,41), U9(1,3), U4(1), U7(1) | 0 V |
| 3V3 | U3(2), U7(2), U2(4) | 3.29 V |
| 5V | U9(2,4), U4(2), RGB1(1) | 5 V |

---

## 二、软件架构

### 启动分派

`app_main()` 第一件事是读唤醒原因，然后走两条完全不同的路径：

```
上电 / 复位 / BOOT 键唤醒        RTC 定时器唤醒
        │                              │
        ▼                              ▼
   run_normal_mode()            run_timer_cycle()
   ├─ 初始化 LCD + 界面           ├─ 不初始化屏幕
   ├─ 初始化 AHT30                ├─ 采 3 次取平均
   ├─ sensor 任务 (1s)            ├─ 连 WiFi、POST webhook
   ├─ WiFi (STA 或 SoftAP)        └─ 重新进入 deep sleep
   ├─ HTTP 服务
   ├─ buttons 任务
   └─ report 任务 (定时上报)
```

关键点：**定时唤醒路径绝不初始化 LCD**，也不创建界面任务，尽可能缩短唤醒时间以省电。

### 任务

| 任务 | 优先级 | 栈 | 职责 |
|------|--------|----|------|
| `sensor` | 4 | 6144 | 每秒读 AHT30 → 刷屏 → 更新 LED |
| `net` | 3 | 4096 | 等待 STA 连接结果，更新联网状态角标 |
| `buttons` | 2 | 3072 | 扫描 IO38 / IO39 / IO0 |
| `report` | 2 | 5120 | 定时上报、断线重连、执行网页休眠请求 |
| `httpd` | 5 | 6144 | 配网页 / 状态页 / API |

### 模块职责

| 模块 | 职责 |
|------|------|
| [`app_core.c`](main/app_core.c) | I²C 初始化、AHT30 采集、派生量计算、干湿分级、最近读数缓存（临界区保护） |
| [`app_display.c`](main/app_display.c) | 帧缓冲绘制、局部擦除、位图 alpha 混合、联网状态角标 |
| [`app_wifi.c`](main/app_wifi.c) | WiFi 状态机、SoftAP 配网、HTTP 服务、webhook 组包与发送 |
| [`app_power.c`](main/app_power.c) | 唤醒原因解析、关屏、休眠源配置、进入 deep sleep |
| [`nvs_storage`](components/nvs_storage/) | 通用 KV 读写（字符串 / u8） |

### 数据流

```
AHT30 ──I²C──> app_core_read()
                   │
                   ├─> app_display_update()  ──> 帧缓冲 ──SPI──> ST7789
                   ├─> app_core_led_set_level() ──RMT──> WS2812B
                   └─> s_last（临界区） ──> HTTP /api/status
                                        └─> app_wifi_report() ──> webhook
```

### 干湿算法

```c
/* 绝对湿度 g/m³ */
es = 6.112 * exp(17.67*T / (T + 243.5));      // 饱和水汽压 hPa
e  = es * RH / 100;
AH = 216.7 * e / (T + 273.15);

/* 露点 °C（Magnus） */
γ  = ln(RH/100) + 17.62*T / (243.12 + T);
Td = 243.12*γ / (17.62 - γ);
```

分级阈值在 `app_config.h`：`RH_DRY_MAX=45`、`RH_NORMAL_MAX=62`、`RH_DAMP_MAX=78`。

### 预计干衣时间

衣物水分蒸发速率正比于剩余含水量，环境稳定时环境相对湿度按指数衰减：

```
RH(t) = RH_floor + A · e^(-k·t)
```

实现见 [`app_core.c`](main/app_core.c) 的 `eta_*` 系列函数：

| 阶段 | 做法 |
|------|------|
| 采样 | 每 60 秒记一个 `(ts, RH, T)`，环形缓冲 96 点（约 1.6 小时） |
| 趋势拟合 | 样本 ≥ 10 时，对 `ln(RH − RH_floor)` 做最小二乘，要求斜率显著为负，得到 k |
| 外推 | `t = ln((RH_now − floor)/(RH_goal − floor)) / k`，目标是历史最低点 + 2 %RH |
| 兜底 | 历史不足时用 VPD：`t(h) = 103 / VPD`，VPD 为饱和水汽压差（hPa） |
| 挂衣识别 | 湿度 1 分钟内突升 > 6 %RH 即清空历史重新记录 |

两条路径都限幅在 48 小时内，超出则上报 `eta_min = -1`（界面显示 `--`）。
`env_reading_t` 里的 `eta_min` / `eta_by_trend` 会一并写进 webhook 与 `/api/status`。

### 低功耗设计要点

1. **只有 GPIO0~21 能唤醒**。所以 IO39 只用来「进入」低功耗，唤醒交给 BOOT 键 + RTC 定时器。
2. **唤醒源配置失败时绝不安睡**：`esp_sleep_enable_timer_wakeup` 与
   `esp_sleep_enable_ext0_wakeup` 都检查返回值，定时器始终兜底，避免原版「睡了叫不醒只能拔电」的问题。
3. **关屏顺序**：先 `app_core_suspend_updates()` 冻结刷新 → 清黑帧缓冲 → `DISPOFF` → 拉低背光。
   否则采集任务会在关屏后立刻把屏幕刷亮。
4. **网页休眠不能直接回调**：HTTP handler 里若直接进深睡会调用 `httpd_stop()` 等待自身返回而死锁，
   因此改为置标志，由 `report` 任务执行。

---

## 三、HTTP 接口

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | AP 模式：配网页；STA 模式：实时状态页（10 秒自动刷新） |
| GET | `/api/status` | 当前读数 JSON |
| GET | `/api/sleep` | 请求进入低功耗 |
| POST | `/save` | 保存 WiFi 与 webhook 配置并重启 |

`/api/status` 返回：

```json
{"device":"dryrack","id":"A1B2C3","valid":true,
 "temp":25.5,"humi":65,"level":"normal","status":"适宜",
 "dew":18.1,"ah":14.2,"cold":false,"wifi":"已联网"}
```

---

## 四、重构记录

### 从「环境监测仪」到「干湿提醒衣架」的改动

| 项 | 改动 |
|----|------|
| SGP30 | 组件整体删除，`app_core` 不再有 IAQ / 基线逻辑，I²C 从双总线简化为单总线 |
| NVS | 从「SGP30 基线存储」改为通用 KV（WiFi 凭据、webhook、格式） |
| 显示 | 从 CO₂/TVOC 仪表盘改为温湿度 + 干湿状态；重新生成全部位图素材 |
| WS2812 | 从「CO₂ 浓度颜色」改为「干湿等级颜色」 |
| 新增 | `app_wifi`（配网 + HTTP + 上报）、`app_power`（低功耗） |
| 依赖 | 移除 `lvgl`（本就未使用）、移除 `espressif/led_strip` |

### 已解决的问题

#### 1. `led_strip` 在 IDF v6.1 下编译失败

`espressif/led_strip` v2.5.5 的 SPI 后端使用了 `heap_caps_calloc` 却没有
`#include "esp_heap_caps.h"`，IDF v6 不再间接包含该头文件。

**修复**：改用原生 RMT 重写 [`ws2812.c`](components/ws2812_led/ws2812.c)，
用 20 MHz 分辨率 + copy 编码器把每个 bit 编码成一个 `rmt_symbol_word_t`
（bit0 = 8/17 tick，bit1 = 16/9 tick），彻底去掉外部组件依赖。

#### 2. `esp_lcd_panel_dev_config_t` 的 `color_space` 字段被移除

IDF v5.2 起用 `rgb_ele_order` 取代 `color_space`。

**修复**：改为 `.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR`。

#### 3. `esp_sleep_get_wakeup_cause()` 在 IDF v6 被废弃

新接口 `esp_sleep_get_wakeup_causes()` 返回 `BIT(cause)` 位图。

**修复**：`app_power.c` 里用 `ESP_IDF_VERSION` 分支封装成统一的位置图判断，兼容 5.x / 6.x。

#### 4. 组件依赖名变化

IDF v6 把驱动拆成细粒度组件，`driver` 不再包含 `driver/i2c_master.h`。

**修复**：`aht30_driver` → `REQUIRES esp_driver_i2c`；
`st7789_display` → `REQUIRES esp_lcd esp_driver_spi esp_driver_gpio`；
`ws2812_led` → `REQUIRES esp_driver_rmt`。

#### 5. 关屏后又被刷亮

采集任务每秒写帧缓冲并 `flush()`，`app_power_enter_sleep()` 关屏后它会把屏幕重新点亮。

**修复**：新增 `app_core_suspend_updates()`，进入低功耗前先冻结刷新。

#### 6. 历史问题（已在原项目中修复）

- I²C 总线分配混乱 → 统一到 `app_config.h` 单一来源
- GPIO19/20 被 USB Serial/JTAG 占用 → `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`
- AHT30 虚假 CRC 警告 → 去除无效校验
- ST7789**V** 黑屏 → 补 VCOM/VRH/VDV/Gamma 初始化序列
- 中文字体乱码 → 弃用 LVGL，改预渲染 4bpp 位图 + alpha 混合

---

## 五、常用操作

### 改干湿阈值

编辑 [`app_config.h`](main/app_config.h)：

```c
#define RH_DRY_MAX      45.0f
#define RH_NORMAL_MAX   62.0f
#define RH_DAMP_MAX     78.0f
#define COLD_TEMP_C     12.0f
```

### 改低功耗间隔

`idf.py menuconfig` → Dry Rack 干湿衣架 → **低功耗模式采样间隔（分钟）**，默认 30。

写入 `sdkconfig` 的 `CONFIG_DRYRACK_SLEEP_INTERVAL_MIN`，`app_config.h` 直接引用。

### 改上报间隔

同上菜单的 **正常模式联网上报间隔（秒）**，默认 300。

### 改显示的文案 / 字号

1. 编辑 [`generate_display_assets.py`](generate_display_assets.py) 的 `ASSETS` 列表
2. `python generate_display_assets.py`
3. 在 `app_display.c` 里用 `BMP(名称)` 引用

### 调试串口

默认走 UART（`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` 关闭了 USB Serial/JTAG）。
看日志：

```bash
idf.py -p COM3 monitor
```

---

## 六、验证清单（烧录后）

- [ ] 启动日志打印「上电 / 复位启动」，屏幕出现标题和温湿度
- [ ] 湿度低于 45 % 时状态显示「干燥」且 WS2812B 变绿
- [ ] 无凭据时手机能搜到 `DryRack-XXXX`，打开 `192.168.4.1` 能配网
- [ ] 配网重启后屏幕右上角变「已联网」，`/api/status` 返回数据
- [ ] 配置 webhook 后每 5 分钟收到推送
- [ ] 长按 IO39 屏幕关闭、日志显示「进入低功耗模式」
- [ ] 等 30 分钟（或临时改成 1 分钟验证）后自动唤醒并推送一次
- [ ] 深睡时按 BOOT 键能亮屏回到正常模式
- [ ] 长按 BOOT 8 秒后重启并重新出现配网热点
