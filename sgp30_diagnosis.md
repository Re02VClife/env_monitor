# SGP30 故障诊断报告

> 日期: 2026-06-15 ~ 2026-06-16

## 最终结果

**旧 SGP30 模块为硬件故障。2026-06-16 更换新模块后一切正常：**

```
I (1001) sgp30: I2C Probe 成功: 在地址 0x58 检测到设备
I (1009) sgp30: SGP30 Feature Set: 0x0022
I (1653) app_core: T=26.1°C  RH=60.4%  CO2=400ppm  TVOC=0ppb
```

> 新模块即插即用，无需修改任何代码。证实旧模块芯片损坏。

---

## 诊断过程

---

## 测试环境

| 项目 | 详情 |
|------|------|
| MCU 板 1 | ESP32-S3-WROOM-1-N16R8 开发板（Octal PSRAM 8MB） |
| MCU 板 2 | Freenove ESP32-S3 WROOM 开发板（Octal PSRAM 8MB） |
| 固件 | ESP-IDF v5.5.3，使用 `i2c_master` 驱动 |
| 对比传感器 | AHT30 温湿度传感器（I²C 地址 0x38）— **正常工作** |
| 待测传感器 | SGP30 气体传感器（I²C 地址 0x58）— **故障** |

---

## 诊断过程

### 测试 1：原板双总线（IO5/6 + IO19/20）

| I²C 总线 | 引脚 | 传感器 | 结果 |
|----------|------|--------|------|
| I2C0 | IO5(SCL), IO6(SDA) | AHT30 (0x38) | ✅ 正常 |
| I2C1 | IO19(SCL), IO20(SDA) | SGP30 (0x58) | ❌ `ESP_ERR_NOT_FOUND` |

> USB Serial/JTAG 已通过 `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` 禁用，GPIO19/20 确认为纯 GPIO。

### 测试 2：交换总线测试

将 SGP30 换到 I2C0（IO5/IO6），AHT30 换到 I2C1（IO19/IO20）。

| 结果 | 说明 |
|------|------|
| AHT30 在两条总线上均正常 | 排除 I²C 控制器和引脚问题 |
| SGP30 在两条总线上均不应答 | SGP30 芯片本身无应答 |

### 测试 3：Freenove 板单总线测试（IO1/IO2）

因 Freenove 板 GPIO47 被 Octal PSRAM DQS 占用、IO5/6 被 CAMA 摄像头接口干扰，改用 IO1/IO2 并简化为单总线架构：

| I²C 总线 | 引脚 | 传感器 | 结果 |
|----------|------|--------|------|
| I2C0 | IO1(SDA), IO2(SCL) | AHT30 (0x38) | ✅ 正常 (25.6°C / 61.2%) |
| I2C0 | IO1(SDA), IO2(SCL) | SGP30 (0x58) | ❌ `ESP_ERR_NOT_FOUND` |

> 两个传感器并接在同一组 SDA/SCL 引脚上，共用 3.3V 和 GND。AHT30 正常应答，SGP30 不应答。

---

## 排除的可能性

| 排查项 | 状态 | 依据 |
|--------|------|------|
| I²C 驱动代码问题 | ❌ 已排除 | AHT30 在相同代码、所有总线上正常工作 |
| I²C 引脚配置错误 | ❌ 已排除 | 测试了 3 组不同引脚(IO5/6, IO19/20, IO1/2) |
| I²C 总线控制器故障 | ❌ 已排除 | 两块板子、两条控制器均测试 |
| USB/JTAG 引脚占用 | ❌ 已排除 | 已配置禁用 |
| I²C 地址错误 | ❌ 已排除 | SGP30 地址 0x58 符合数据手册 |
| 供电问题 | ❌ 已排除 | 3.3V 供电正常（AHT30 同电源工作） |
| 上拉电阻不足 | ❌ 已排除 | ESP32 内部上拉对 AHT30 有效，且两块板均测试 |
| MCU 板故障 | ❌ 已排除 | 在两块不同的 ESP32-S3 板上测试 |

---

## 诊断代码

`i2c_master_probe()` 是 ESP-IDF I²C 驱动最底层的探测函数，发送设备地址后等待 ACK 应答。SGP30 在该调用中返回 `ESP_ERR_NOT_FOUND`，表示芯片在电气层面完全没有应答。

```c
// sgp30_init() 中的关键诊断代码
ret = i2c_master_probe(bus_handle, 0x58, 200);  // 200ms 超时
if (ret == ESP_OK) {
    ESP_LOGI(TAG, "I2C Probe 成功");
} else {
    ESP_LOGE(TAG, "I2C Probe 失败: 地址 0x58 无设备应答！");
    return ESP_ERR_NOT_FOUND;
}
```

---

## 完整启动日志

```
I (989)  app_core: I2C 总线初始化完成 (SDA=IO1, SCL=IO2)
I (994)  nvs: NVS 存储就绪
I (997)  aht30: AHT30 已校准 (status=0x18)              ← AHT30 正常
E (1001) sgp30: I2C Probe 失败: 地址 0x58 无设备应答！   ← SGP30 故障
W (1012) app_core: SGP30 初始化失败: ESP_ERR_NOT_FOUND
I (1126) app_core: T=25.7°C  RH=61.2%  CO2=0ppm  TVOC=0ppb
```

> AHT30 在同一 I²C 总线上稳定输出 25.7°C / 61.2%，SGP30 probe 直接返回不应答。

---

## 结论

SGP30 模块在多种硬件配置下均无法在 I²C 总线上应答其地址 0x58，属于**模块硬件故障**。请更换。
