/**
 * Roon Cover Display
 * ==================
 * 硬件: Waveshare ESP32-S3-Touch-LCD-4 V4 (非触摸版)
 * 屏幕: ST7701, 480×480, RGB 并行接口
 * 功能: 连接 RoonCoverArt 服务, 全屏显示当前播放专辑封面
 *       任意分辨率自动缩放, 切歌实时刷新, 断线自动重连
 *
 * 依赖库 (Arduino IDE 库管理器安装):
 *   - LovyanGFX  (by lovyan03)
 *   - ArduinoJson (v7.x)
 *
 * Arduino IDE 配置:
 *   Board: ESP32S3 Dev Module
 *   PSRAM: Enable
 *   USB CDC On Boot: Enabled
 *   Partition Scheme: Huge APP (3MB NO OTA/1MB SPIFFS)
 *
 * 详见 README.md 和 docs/pinout.md
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

// ===== 用户配置 (修改这 4 行为你的网络信息) =====
const char* WIFI_SSID     = "storm";
const char* WIFI_PASS     = "STORM251280";
const char* SERVER_HOST   = "192.168.1.224";
const int   SERVER_PORT   = 3666;

// ===== 系统参数 =====
const int   POLL_INTERVAL = 200;    // 轮询间隔 (ms), 200=近乎实时
const int   WIFI_RETRY_MS = 3000;   // WiFi 重试间隔 (ms)

// ===== TCA9554 IO 扩展器 (V4: I2C addr=0x24, SDA=GPIO15, SCL=GPIO7) =====
void tca9554_init() {
  Wire.begin(15, 7);
  Wire.setClock(400000);
  // 先写输出寄存器 (全部拉高), 再写方向寄存器 (0x3A)
  Wire.beginTransmission(0x24);
  Wire.write(0x02);
  Wire.write(0xff);
  Wire.endTransmission();
  Wire.beginTransmission(0x24);
  Wire.write(0x03);
  Wire.write(0x3a);
  Wire.endTransmission();
}

// ===== ST7701 显示屏 (3-wire SPI 初始化 + RGB 并行数据) =====
class LGFX : public lgfx::LGFX_Device {
public:
  lgfx::Bus_RGB      _bus;
  lgfx::Panel_ST7701 _panel;
  LGFX() {
    {
      auto p = _panel.config();
      p.memory_width  = 480;
      p.memory_height = 480;
      p.panel_width   = 480;
      p.panel_height  = 480;
      p.offset_x = 0;
      p.offset_y = 0;
      p.offset_rotation = 0;
      p.readable  = false;
      p.invert    = false;
      p.rgb_order = false;
      _panel.config(p);
    }
    {
      auto d = _panel.config_detail();
      d.pin_cs   = 42;
      d.pin_sclk = 2;
      d.pin_mosi = 1;
      d.use_psram = 2;
      _panel.config_detail(d);
    }
    {
      auto c = _bus.config();
      c.panel = &_panel;
      // RGB565 数据线
      c.pin_d0=5;   c.pin_d1=45;  c.pin_d2=48;  c.pin_d3=47;  c.pin_d4=21;
      c.pin_d5=14;  c.pin_d6=13;  c.pin_d7=12;  c.pin_d8=11;  c.pin_d9=10;  c.pin_d10=9;
      c.pin_d11=46; c.pin_d12=3;  c.pin_d13=8;  c.pin_d14=18; c.pin_d15=17;
      // 同步信号
      c.pin_henable = 40;
      c.pin_vsync   = 39;
      c.pin_hsync   = 38;
      c.pin_pclk    = 41;
      // 时序参数
      c.freq_write       = 12000000;
      c.pclk_active_neg  = 0;
      c.hsync_polarity   = 1;
      c.hsync_front_porch = 10;
      c.hsync_pulse_width = 8;
      c.hsync_back_porch  = 50;
      c.vsync_polarity   = 1;
      c.vsync_front_porch = 10;
      c.vsync_pulse_width = 8;
      c.vsync_back_porch  = 20;
      c.pclk_idle_high   = 0;
      c.de_idle_high     = 1;
      _bus.config(c);
    }
    _panel.setBus(&_bus);
    setPanel(&_panel);
  }
};

LGFX display;
String       prevKey = "";
unsigned long lastPoll = 0;

// ===== 右上角状态提示 =====
void showRightTop(const char* msg, uint16_t color) {
  display.setTextSize(2);
  display.setTextColor(color);
  display.setTextDatum(TR_DATUM);
  display.fillRect(0, 0, 480, 22, TFT_BLACK);
  display.drawString(msg, 476, 2);
}

// ===== URL 编码 =====
String urlEncode(const String& s) {
  String e;
  e.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
      e += c;
    else if (c == ' ')
      e += "%20";
    else {
      char b[4];
      snprintf(b, sizeof(b), "%%%02X", (unsigned char)c);
      e += b;
    }
  }
  return e;
}

// ===== WiFi 连接 =====
void connectWiFi() {
  display.fillScreen(TFT_BLACK);
  showRightTop("WiFi...", TFT_WHITE);
  Serial.print("WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(WIFI_RETRY_MS);
    Serial.print(".");
  }
  Serial.println("\nIP: " + WiFi.localIP().toString());
  showRightTop("WiFi OK", TFT_GREEN);
  delay(1000);
  display.fillScreen(TFT_BLACK);
}

// ===== 下载 JPEG → 解码缩放 → 全屏显示 =====
bool downloadImage(const String& url) {
  HTTPClient http;
  http.begin(url);
  http.setTimeout(15000);
  if (http.GET() != 200) {
    Serial.printf("HTTP %d\n", http.GET());
    http.end();
    return false;
  }
  int len = http.getSize();
  WiFiClient* s = http.getStreamPtr();
  size_t sz = (len > 0) ? len : (6UL * 1024 * 1024);
  uint8_t* buf = (uint8_t*)ps_malloc(sz);
  if (!buf) { http.end(); return false; }
  size_t pos = 0;
  while (s->connected() || s->available()) {
    while (s->available() && pos < sz) buf[pos++] = s->read();
    if (pos >= sz) break;
    delay(1);
  }
  if (pos == 0) { free(buf); http.end(); return false; }
  // 强制拉伸填满 480×480 (末尾 4 个 0 = 自动检测 JPEG 原始尺寸)
  display.drawJpg(buf, pos, 0, 0, 480, 480, 0, 0, 0, 0);
  free(buf);
  http.end();
  Serial.printf("Cover %d bytes\n", pos);
  return true;
}

// ===== setup =====
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nRoon Cover Display");

  // 1. TCA9554
  tca9554_init();

  // 2. 显示屏
  display.init();
  display.setBrightness(255);

  // 3. 启动画面
  display.fillScreen(TFT_BLACK);
  display.setTextSize(3);
  display.setTextColor(TFT_WHITE);
  display.setTextDatum(MC_DATUM);
  display.drawString("roon display", 240, 240);
  delay(3000);
  display.fillScreen(TFT_BLACK);

  // 4. WiFi
  connectWiFi();
  display.fillScreen(TFT_BLACK);

  // 5. 等待 Roon 服务器
  while (true) {
    showRightTop("Roon...", TFT_YELLOW);
    HTTPClient http;
    String u = String("http://") + SERVER_HOST + ":" + SERVER_PORT + "/api/status";
    http.begin(u);
    http.setTimeout(5000);
    if (http.GET() == 200) {
      String json = http.getString();
      http.end();
      JsonDocument doc;
      if (!deserializeJson(doc, json)) {
        const char* key = doc["image_key"];
        if (key && strlen(key)) {
          prevKey = String(key);
          String imgUrl = String("http://") + SERVER_HOST + ":" + SERVER_PORT
                        + "/roonapi/getImage?image_key=" + key;
          const char* album = doc["three_line"]["line3"];
          if (album && strlen(album))
            imgUrl += "&albumName=" + urlEncode(String(album));
          if (downloadImage(imgUrl)) break;
        }
      }
    } else {
      http.end();
    }
    delay(2000);
  }
  Serial.println("Ready.");
}

// ===== loop =====
void loop() {
  unsigned long now = millis();
  if (now - lastPoll < POLL_INTERVAL) { delay(10); return; }
  lastPoll = now;

  if (WiFi.status() != WL_CONNECTED) {
    display.fillScreen(TFT_BLACK);
    connectWiFi();
    display.fillScreen(TFT_BLACK);
    return;
  }

  String su = String("http://") + SERVER_HOST + ":" + SERVER_PORT + "/api/status";
  HTTPClient http;
  http.begin(su);
  http.setTimeout(5000);

  if (http.GET() != 200) {
    http.end();
    display.fillScreen(TFT_BLACK);
    showRightTop("Roon...", TFT_YELLOW);
    return;
  }

  String js = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, js)) return;

  const char* key = doc["image_key"];
  if (!key || !strlen(key)) return;
  if (prevKey == String(key)) return;

  prevKey = String(key);

  String imgUrl = String("http://") + SERVER_HOST + ":" + SERVER_PORT
                + "/roonapi/getImage?image_key=" + key;
  const char* album = doc["three_line"]["line3"];
  if (album && strlen(album))
    imgUrl += "&albumName=" + urlEncode(String(album));

  if (!downloadImage(imgUrl)) {
    display.fillScreen(TFT_BLACK);
    showRightTop("Roon...", TFT_YELLOW);
  }
}
