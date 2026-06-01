# Roon Cover Display — ESP32-S3 桌面专辑封面显示器

基于微雪 ESP32-S3-Touch-LCD-4 (V4 非触摸版) 的 Roon 专辑封面桌面小摆件。连接局域网内的 RoonCoverArt 服务器，在 480×480 屏幕上实时显示当前播放曲目的专辑封面。

## 效果

- 上电 → 启动画面 "roon display" → 自动连接 WiFi → 连接 Roon 服务器 → 全屏显示封面
- 切歌自动刷新（200ms 检测延迟）
- 封面自适应缩放到 480×480
- 服务器离线/断网自动重连

## 硬件

| 项目 | 参数 |
|------|------|
| 开发板 | 微雪 ESP32-S3-Touch-LCD-4 **V4 非触摸版** |
| 处理器 | ESP32-S3-N16R8, 双核 240MHz |
| 屏幕 | 4 寸 TFT, 480×480, ST7701 驱动 (RGB 并行接口) |
| 存储 | 16MB Flash + 8MB PSRAM |
| 连接 | WiFi 2.4GHz |

> ⚠️ **重要：V1 (触摸版) 和 V4 (非触摸版) 的引脚分配完全不同。** 本代码仅适用于 V4 版。

> ⚠️ 开发板wiki：https://www.waveshare.net/wiki/ESP32-S3-Touch-LCD-4

## 依赖服务

需要局域网内有 [RoonCoverArt](https://github.com/epochaudio/RoonCoverArt_Square_Frame_Docker) 服务器运行。

## 快速开始

### 1. 安装 Arduino 库

Arduino IDE → 工具 → 管理库，搜索安装：

- **LovyanGFX** (by lovyan03)
- **ArduinoJson** (v7.x)

### 2. Arduino IDE 配置

```
工具 → 开发板 → ESP32S3 Dev Module
工具 → PSRAM → Enable
工具 → USB CDC On Boot → Enabled
工具 → Partition Scheme → Huge APP (3MB NO OTA/1MB SPIFFS)
```

### 3. 修改配置

打开 `RoonCoverDisplay.ino`，修改顶部 4 行为你的网络信息：

```cpp
const char* WIFI_SSID     = "你的WiFi名";
const char* WIFI_PASS     = "你的WiFi密码";
const char* SERVER_HOST   = "X.X.X.X";   // RoonCoverArt 服务器的 IP
const int   SERVER_PORT   = 3666;             // RoonCoverArt 端口
```

### 4. 上传

编译上传到开发板即可。

## 工作原理

```
上电
  ↓
初始化 TCA9554 (I2C: GPIO15/GPIO7, addr=0x24)
  ↓
初始化 ST7701 显示屏 (3-wire SPI: CS=42, SCK=2, MOSI=1 + RGB 并行总线)
  ↓
显示启动画面 "roon display"
  ↓
WiFi 连接
  ↓
每 200ms 轮询 GET /api/status → 提取 image_key
  ↓
image_key 变化 → GET /roonapi/getImage → 下载 JPEG
  → TJpgDec 解码 + 缩放到 480×480 → 全屏显示
```

## 项目结构

```
RoonCoverDisplay/
├── RoonCoverDisplay.ino    # 主程序
├── README.md               # 项目说明
└── docs/
    ├── pinout.md            # V4 引脚映射表
    └── troubleshooting.md   # 踩坑记录和解决方案
```

## API 说明

RoonCoverArt 项目提供的 API 端点：

| 端点 | 说明 |
|------|------|
| `GET /api/status` | 当前播放状态 (返回 JSON, 含 `image_key`) |
| `GET /roonapi/getImage?image_key=xxx&albumName=xxx` | 获取专辑封面 (JPEG) |

`/api/status` 返回 JSON 示例：

```json
{
  "connected": true,
  "is_playing": true,
  "image_key": "10b0f32afee89349d1fffd54914f5bc1",
  "three_line": {
    "line1": "歌曲名",
    "line2": "艺术家",
    "line3": "专辑名"
  }
}
```

## License

MIT
