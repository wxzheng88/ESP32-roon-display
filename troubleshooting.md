# 踩坑记录

> 坑 1-8 继承自 v1 (引脚/编译相关)。坑 9+ 是 v2 新增。

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

## 坑 9 (v2): 切歌延迟 30-60s

### 现象
v1 代码在服务器返回新 `image_key` 后 30-60s 才显示新封面，但 RoonCoverArt 网页端是即时的。

### 原因
v1 的 `downloadImage()` 是**同步阻塞**调用。loop() 在下载旧图期间完全停顿：
```
T=0s    播放 A, 开始下载 A (假设耗时 5s)
T=2s    用户切到 B  ← 此时 loop() 还在下载 A
T=5s    A 下载完, 显示 A (过时)
T=5.1s  轮询, 发现 image_key=bbb, 启动下载 B
T=10.1s 显示 B
```
如果服务器 `image_key` 又是懒更新（要等抓到图才更新 `image_key`），则感知延迟 = 服务器抓图时间 + 本地下载 = 30-60s。

### 解决 (v2)
1. **非阻塞下载状态机** — `tickDownload()` 每帧推进，loop() 永不阻塞
2. **新图 abort 旧下载** — `startDownload()` 检测到正在下载时立即 `abortDownload()`
3. **30s 总超时** — 防止服务器 stall
4. **歌名作为变化信号** — 不止 `image_key`，`three_line.line1` 变化也触发

## 坑 10 (v2): `s->read()` 返回 -1 污染 buffer

### 现象
v1 `downloadImage()` 的 `while(s->available()&&pos<sz)buf[pos++]=s->read();` 在某些情况下会把 -1 (0xFF) 写入 JPEG 数据，导致解码失败或显示乱图。

### 原因
`s->read()` 在流没有数据但 `available()` 返回非零的边角情况可能返回 -1。

### 解决 (v2)
```cpp
int c = dlStream->read();
if (c >= 0) dlBuf[dlPos++] = (uint8_t)c;
```

## 坑 11 (v2): NTP 首次同步延迟

### 现象
上电后时钟模式迟迟不显示时间，显示 1970 或空白。

### 原因
`configTime()` 异步发起 NTP 请求，但 `time()` 立即调用会返回 0 (1970-01-01) 直到同步完成。

### 解决
代码中已检查 `if (t < 1700000000) return;`（1700000000 = 2023-11-14），NTP 未同步则不绘制。同步通常 1-3 秒完成。

## 坑 12 (v2): U8g2 默认字体无 CJK

### 现象
用 `display.drawString()` 写中文显示成方块 □。

### 原因
LovyanGFX 默认字体是 6x8 ASCII 位图，无中文。

### 解决
安装 U8g2 + U8g2_for_Adafruit_GFX，指定中文字体：
```cpp
u8g2.setFont(u8g2_font_wqy12_t_chinese3);  // 文泉驿 12px, ~50KB Flash
u8g2.setForegroundColor(0xBDF7);
u8g2.setCursor(x, y);
u8g2.print("2026年6月24日 周三");
```

## 坑 13 (v2): `getUTF8Width` 与 `setCursor` 单位

### 现象
居中绘制中文日期时位置偏差。

### 原因
`getUTF8Width()` 返回的是**像素宽度**（已计入 CJK 全角宽度 = 2x ASCII 宽度），与 `setCursor(x, y)` 像素单位一致。但 `getUTF8Width` 返回 `uint16_t`，最大 65535，对 480×480 屏幕够用。

### 解决
直接用像素相减：
```cpp
uint16_t w = u8g2.getUTF8Width(dateStr);
u8g2.setCursor((480 - w) / 2, 340);
```
