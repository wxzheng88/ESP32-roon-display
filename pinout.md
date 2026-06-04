# 引脚映射 — ESP32-S3-Touch-LCD-4 V4 (非触摸版)

> ⚠️ V1 (触摸版) 和 V4 引脚完全不同，请确认你的板子版本。

## TCA9554 IO 扩展器

| 信号 | GPIO |
|------|------|
| I2C SDA | **GPIO15** |
| I2C SCL | **GPIO7** |
| I2C 地址 | **0x24** |

TCA9554 寄存器操作：

| EXIO | 功能 |
|------|------|
| EXIO0 | TP_RST (V4 未使用) |
| EXIO1 | 输出 (背光相关, 由 0x02 寄存器初始化) |
| EXIO2 | 输出 |
| EXIO3 | SD_CS (输入, V4 由 0x3A 方向寄存器配置为输入) |
| EXIO4 | 输出 |

初始化顺序（来自 V4 官方 HelloWorld 例程）：
1. 写 `0x02` (输出寄存器) = `0xFF` (全部拉高)
2. 写 `0x03` (方向寄存器) = `0x3A` (0011 1010: EXIO0/3/5/6/7=输入, 其余=输出)

## ST7701 LCD (3-wire SPI 初始化)

| 信号 | GPIO |
|------|------|
| CS | **GPIO42** |
| SCK | **GPIO2** |
| MOSI (SDA) | **GPIO1** |

> 初始化使用软件 3-wire 9-bit SPI (D/C 位嵌入数据流), 由 LovyanGFX 自动处理

## ST7701 LCD (RGB 并行数据总线)

### 同步信号

| 信号 | GPIO |
|------|------|
| DE (Data Enable) | **GPIO40** |
| VSYNC | **GPIO39** |
| HSYNC | **GPIO38** |
| PCLK | **GPIO41** |

### 数据线 (RGB565)

| 位 | 信号 | GPIO |
|------|------|------|
| B0 | B1 | GPIO5 |
| B1 | B2 | GPIO45 |
| B2 | B3 | GPIO48 |
| B3 | B4 | GPIO47 |
| B4 | B5 | GPIO21 |
| G0 | G0 | GPIO14 |
| G1 | G1 | GPIO13 |
| G2 | G2 | GPIO12 |
| G3 | G3 | GPIO11 |
| G4 | G4 | GPIO10 |
| G5 | G5 | GPIO9 |
| R0 | R1 | GPIO46 |
| R1 | R2 | GPIO3 |
| R2 | R3 | GPIO8 |
| R3 | R4 | GPIO18 |
| R4 | R5 | GPIO17 |

### RGB 时序参数

| 参数 | 值 |
|------|-----|
| PCLK 频率 | 12 MHz |
| PCLK 采样沿 | 上升沿 |
| HSYNC 极性 | 高有效 |
| HSYNC 前肩 | 10 |
| HSYNC 脉宽 | 8 |
| HSYNC 后肩 | 50 |
| VSYNC 极性 | 高有效 |
| VSYNC 前肩 | 10 |
| VSYNC 脉宽 | 8 |
| VSYNC 后肩 | 20 |

## V1 vs V4 差异速查

| 项目 | V1 (触摸版) | V4 (非触摸版) |
|------|------------|------------|
| I2C SDA | GPIO8 | **GPIO15** |
| I2C SCL | GPIO9 | **GPIO7** |
| TCA9554 地址 | 0x20 | **0x24** |
| 触摸芯片 | GT911 | 无 |
| 示例代码 | Arduino-v3.0.7 | Arduino-v3.3.2 |
