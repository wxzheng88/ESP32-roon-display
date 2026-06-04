# 踩坑记录

## 坑 1: V1 vs V4 引脚不同 (耗时最长)

### 现象
屏幕只有背光，没有任何画面。所有库 (LovyanGFX, ESP32_Display_Panel, GFX_Library_for_Arduino) 初始化都"成功"但屏幕不显示。

### 原因
微雪 ESP32-S3-Touch-LCD-4 有两个硬件版本：
- **V1 (触摸版)**: TCA9554 在 I2C addr=0x20, SDA=GPIO8, SCL=GPIO9
- **V4 (非触摸版)**: TCA9554 在 I2C addr=0x24, SDA=GPIO15, SCL=GPIO7

Wiki 上的引脚表是 V1 的。V4 的 TCA9554 控制着 LCD_RST 和背光，地址/引脚错了 → 芯片从未初始化 → ST7701 不复位 → 屏幕永远不亮。

### 解决
使用 V4 官方 HelloWorld 例程验证硬件，确认正确的 I2C 地址和引脚。关键代码：

```cpp
Wire.begin(15, 7);   // V4 用 GPIO15(SDA), GPIO7(SCL)
Wire.beginTransmission(0x24);  // V4 地址 0x24
Wire.write(0x02); Wire.write(0xff);
Wire.write(0x03); Wire.write(0x3a);
```

## 坑 2: GFX_Library_for_Arduino 编译失败

### 现象
```
error: 'ESP_INTR_CPU_AFFINITY_AUTO' was not declared
```

### 原因
中文版 arduino-esp32 (3.0.7-cn) 中宏名不同。

### 解决
修改 `Arduino_ESP32QSPI.cpp:59`：
```cpp
// 旧: .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
// 新: .isr_cpu_id = INTR_CPU_ID_AUTO,
```

## 坑 3: LovyanGFX Panel_ST7701 编译失败

### 现象
```
error: 'Panel_ST7701' in namespace 'lgfx' does not name a type
```

### 原因
ST7701 使用 RGB 并行接口，需要手动包含平台专用头文件。

### 解决
```cpp
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
```

## 坑 4: PCLK 采样沿错误

### 现象
屏幕初始化在串口显示成功，但画面不动。

### 原因
LovyanGFX Bus_RGB 默认 `pclk_active_neg=1` (下降沿采样)，但 ST7701 需要上升沿。

### 解决
```cpp
cfg.pclk_active_neg = 0;  // 上升沿采样
```

## 坑 5: TCA9554 状态锁存

### 现象
刷入新程序后屏幕不亮，但串口显示一切正常。即使擦除 ESP32 Flash 也无济于事。

### 原因
TCA9554 是独立芯片，ESP32 擦除 Flash 不会清除其输出锁存器。错误的 I2C 命令可能写入了让背光/RST 失效的状态。

### 解决
拔掉 USB 完全断电 30-60 秒，让 TCA9554 的电容放电恢复默认状态。

## 坑 6: `drawJpg` 不缩放

### 现象
大分辨率封面 (如 3000×3000) 在 480×480 屏幕上只显示左上角。

### 原因
某些 LovyanGFX 版本的 `drawJpg(buf, len, x, y, w, h)` 仅裁剪不缩放。

### 解决
使用 10 参数版本强制缩放：
```cpp
display.drawJpg(buf, pos, 0, 0, 480, 480, 0, 0, 0, 0);
//              src 数据    目标区域(全屏)   源区域(0=自动检测原图尺寸)
```

## 坑 7: `exp` 变量名冲突

### 现象
```cpp
ESP_IOExpander *exp = nullptr;
// error: 'exp' redeclared as different kind of entity
```

### 原因
`exp` 是 C 标准库 `<math.h>` 中的指数函数，和全局变量名冲突。

### 解决
重命名为 `ioex` 或其他不冲突的名字。

## 坑 8: `/api/status` JSON 字段名

### 现象
`image_key` 能解析，但 `albumName` 总是空。

### 原因
RoonCoverArt 的 API 返回的专辑名在 `three_line.line3`，而不是 `album_name`/`albumName`。

### 解决
```cpp
const char* album = doc["three_line"]["line3"];
```
