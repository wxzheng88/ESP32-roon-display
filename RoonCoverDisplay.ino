/**
 * Roon Cover Display v3 (稳定性增强版)
 * =====================================
 * Hardware: Waveshare ESP32-S3-Touch-LCD-4 V4 (non-touch)
 * Display:  ST7701, 480x480, RGB parallel
 *
 * v3 改动（外观和硬件配置完全不变，只修稳定性）：
 *   1. WiFi 断线不再死等：原来 loop 里 while(WiFi未连接) delay(3000) 会把
 *      整个程序卡死在那一行，时钟也不走；改为非阻塞自动重连。
 *   2. 开机不再死机：原来开机时如果 Roon 服务器不在线，重试 3 次后程序
 *      会永远停在 "Offline" 黑屏；现在改为持续自动重试。
 *   3. NTP 服务器换成国内源（阿里云），pool.ntp.org 在国内经常连不上，
 *      这是"时钟显示不出来/黑屏"的常见原因。
 *   4. 轮询从 200ms 放宽到 1000ms，并复用 TCP 连接：大幅减轻路由器和
 *      服务器的压力，长时间运行更稳。
 *   5. 图片下载改为 4KB 块读取（原来逐字节读，太慢），并增加"卡死检测"
 *      （8 秒收不到数据自动放弃重试）。
 *   6. 下载完成后校验 JPEG 文件头，坏数据不再往屏幕上画。
 *   7. 下载失败会自动重试（原来失败后同一封面永远不会重试）。
 *   8. 自愈机制：连续 10 分钟联系不上服务器（含 WiFi 假死）自动重启。
 *   9. 关闭 WiFi 休眠(setSleep)、禁止反复写flash(persistent)，均提升稳定性。
 *
 * v3.1 改动（电源按键）：
 *   通过 V4 原理图确认：BAT-PWR 按键信号(SYS_OUT)接在 ESP32 的 GPIO16
 *   上（按下=低电平，板上已有10K上拉电阻），程序可以读取按键。
 *   现已实现：长按 BAT-PWR 3秒 = 关机：
 *     - 电池供电：写 I2C 把 SYS_EN 拉低 → 真正断电关机；
 *     - USB 供电：电路设计上 USB 电无法被切断 → 转为深度待机（屏幕熄灭，
 *       功耗降到最低），再按 BAT-PWR 立即开机。
 *   开机后前 10 秒忽略按键（防止"按按键唤醒后手没松开又被关掉"）。
 *   RESET 按键 = 硬件复位（EN脚），与程序无关，任何时候按都能重启。
 *   短按开机 = 硬件行为（板上电源芯片），程序无需参与。
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <time.h>
#include <esp_sleep.h>
#include <U8g2_for_Adafruit_GFX.h>

#include "boot_logo.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

#include "wifi_config.h"

// ===================== 配置 =====================
const int   POLL_INTERVAL         = 1000;      // 服务器轮询间隔（原200，太快伤路由器）
const int   IDLE_TO_CLOCK_MS      = 15000;     // 停止播放多久后切换到时钟
const long  DL_TOTAL_TIMEOUT_MS   = 30000;     // 单张图片下载总超时
const long  DL_STALL_MS           = 8000;      // 下载中途收不到数据的超时
const size_t DL_CHUNK             = 4096;      // 下载块大小
const int   WIFI_REKICK_MS        = 15000;     // WiFi 连不上时，多久重新发起一次
const long  NO_CONTACT_RESTART_MS = 600000;    // 10分钟完全失联→自动重启自愈
const long  GMT_OFFSET_SEC        = 8 * 3600;
const char* NTP_SERVER            = "ntp.aliyun.com";  // 国内源，备选在 configTime 里

// ---- 电源按键（BAT-PWR）----
const int          PWR_BTN_GPIO      = 16;    // 原理图: SYS_OUT → GPIO16, 按下=低电平
const unsigned long PWR_OFF_HOLD_MS  = 3000;  // 长按3秒=关机
const unsigned long PWR_BOOT_GRACE_MS= 10000; // 开机后10秒内忽略按键(防唤醒误关)
const unsigned long BATT_LOG_MS      = 60000; // 电池电压串口打印间隔

// ===================== 前置声明 =====================
enum AppState { STATE_COVER, STATE_CLOCK };
struct StatusInfo { bool ok; bool isPlaying; String imageKey; String title; String album; };
void enterState(AppState s);
void showStatus(const char* msg, uint16_t color);
void showLogo();
String urlEncode(const String& s);
void abortDownload();
void startDownload(const String& url);
bool tickDownload();
StatusInfo pollStatus();
String buildImageUrl(const String& key, const String& album);
void showClock();
void wifiKick();
void powerOffNow();

// ===================== 全局状态 =====================
AppState          state              = STATE_COVER;
unsigned long     stateEnteredMs     = 0;
unsigned long     lastPlayingSeenMs  = 0;
unsigned long     lastClockTickMs    = 0;
int               lastClockMinute    = -1;
String            prevKey            = "";
String            prevTitle          = "";
unsigned long     lastPoll           = 0;
bool              everPolled         = false;   // 开机后是否成功问过一次服务器
unsigned long     lastServerOkMs     = 0;       // 最近一次服务器正常的时间（自愈用）

// WiFi 非阻塞重连
bool              wifiConnecting     = false;
unsigned long     wifiStartMs        = 0;

// 电源按键
unsigned long     btnDownSince       = 0;
bool              btnWasDown         = false;
unsigned long     setupDoneMs        = 0;    // 开机完成时刻（按键宽限用）
unsigned long     lastBattLogMs      = 0;

// 图片下载
enum DlState { DL_IDLE, DL_STREAM, DL_DECODE };
DlState           dlState     = DL_IDLE;
HTTPClient*       dlHttp      = nullptr;
WiFiClient*       dlStream    = nullptr;
uint8_t*          dlBuf       = nullptr;
size_t            dlPos       = 0;
size_t            dlSize      = 0;
unsigned long     dlStartMs   = 0;
unsigned long     dlLastDataMs= 0;
size_t            dlLastPos   = 0;
bool              dlSuccess   = false;       // 本次下载是否真正成功（画上了图）

const char* WK_CN[] = {"周日","周一","周二","周三","周四","周五","周六"};

// ===================== TCA9554 =====================
void tca9554_init() {
  Wire.begin(15, 7); Wire.setClock(400000);
  Wire.beginTransmission(0x24); Wire.write(0x02); Wire.write(0xff); Wire.endTransmission();
  Wire.beginTransmission(0x24); Wire.write(0x03); Wire.write(0x3a); Wire.endTransmission();
}

// ===================== 电源管理（板载 CH32V003 协处理器, I2C 0x24） =====================
// 原理图事实（V4.0）：
//   - BAT-PWR 按键 → SYS_OUT 节点 → 同时接 ESP32 GPIO16 和 CH32（按下=低电平）
//   - SYS_EN = 输出寄存器(0x03) 的 bit5：拉低 → Q4 断开电池 → 关机。
//     仅电池供电时真正断电；USB 供电走独立通路，任何软件都无法切断（电路设计如此）。
//   - 出厂手势（板上协处理器固件，与程序无关）：电池供电时"双击 BAT-PWR = 直接断电"。
static bool ch32WriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x24);
  Wire.write(reg); Wire.write(val);
  return Wire.endTransmission() == 0;
}

static uint8_t ch32ReadReg(uint8_t reg) {
  Wire.beginTransmission(0x24);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom((uint8_t)0x24, (uint8_t)1) != (uint8_t)1) return 0xFF;
  return Wire.read();
}

float ch32ReadBatteryV() {   // 寄存器0x06: 2字节小端ADC, 电压 = raw/1023*3.3*3.0
  Wire.beginTransmission(0x24);
  Wire.write(0x06);
  if (Wire.endTransmission(false) != 0) return -1.0f;
  if (Wire.requestFrom((uint8_t)0x24, (uint8_t)2) != (uint8_t)2) return -1.0f;
  uint16_t lo = Wire.read(); uint16_t hi = Wire.read();
  uint16_t raw = ((uint16_t)hi << 8) | lo;
  return raw / 1023.0f * 3.3f * 3.0f;
}

// powerOffNow() 定义在文件下方（display 对象声明之后，因为要用到屏幕）

// ===================== 显示驱动（硬件配置，与v2完全一致，勿改） =====================
class LGFX : public lgfx::LGFX_Device
{
public:
  lgfx::Bus_RGB   _bus_instance;
  lgfx::Panel_ST7701 _panel_instance;
  LGFX(void)
  {
    {
      auto panel_cfg = _panel_instance.config();
      panel_cfg.memory_width  = 480;
      panel_cfg.memory_height = 480;
      panel_cfg.panel_width   = 480;
      panel_cfg.panel_height  = 480;
      panel_cfg.offset_x      = 0;
      panel_cfg.offset_y      = 0;
      panel_cfg.offset_rotation = 0;
      panel_cfg.readable      = false;
      panel_cfg.invert        = false;
      panel_cfg.rgb_order     = false;
      _panel_instance.config(panel_cfg);
    }
    {
      auto panel_detail = _panel_instance.config_detail();
      panel_detail.pin_cs   = 42;
      panel_detail.pin_sclk = 2;
      panel_detail.pin_mosi = 1;
      panel_detail.use_psram = 2;
      _panel_instance.config_detail(panel_detail);
    }
    {
      auto bus_cfg = _bus_instance.config();
      bus_cfg.panel       = &_panel_instance;
      bus_cfg.pin_d0      = 5;   bus_cfg.pin_d1      = 45;
      bus_cfg.pin_d2      = 48;  bus_cfg.pin_d3      = 47;
      bus_cfg.pin_d4      = 21;  bus_cfg.pin_d5      = 14;
      bus_cfg.pin_d6      = 13;  bus_cfg.pin_d7      = 12;
      bus_cfg.pin_d8      = 11;  bus_cfg.pin_d9      = 10;
      bus_cfg.pin_d10     = 9;   bus_cfg.pin_d11     = 46;
      bus_cfg.pin_d12     = 3;   bus_cfg.pin_d13     = 8;
      bus_cfg.pin_d14     = 18;  bus_cfg.pin_d15     = 17;
      bus_cfg.pin_henable = 40;  bus_cfg.pin_vsync   = 39;
      bus_cfg.pin_hsync   = 38;  bus_cfg.pin_pclk    = 41;
      bus_cfg.freq_write  = 12000000;
      bus_cfg.pclk_active_neg = 0;
      bus_cfg.hsync_polarity      = 1;
      bus_cfg.hsync_front_porch   = 10;
      bus_cfg.hsync_pulse_width   = 8;
      bus_cfg.hsync_back_porch    = 50;
      bus_cfg.vsync_polarity      = 1;
      bus_cfg.vsync_front_porch   = 10;
      bus_cfg.vsync_pulse_width   = 8;
      bus_cfg.vsync_back_porch    = 20;
      bus_cfg.pclk_idle_high      = 0;
      bus_cfg.de_idle_high        = 1;
      _bus_instance.config(bus_cfg);
    }
    _panel_instance.setBus(&_bus_instance);
    setPanel(&_panel_instance);
  }
};

static LGFX display;

class GFXAdapter : public Adafruit_GFX
{
  LGFX* _lgfx;
public:
  GFXAdapter(LGFX* lgfx) : Adafruit_GFX(480, 480), _lgfx(lgfx) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override
  {
    _lgfx->drawPixel((int32_t)x, (int32_t)y, color);
  }
};

static GFXAdapter gfxAdapter(&display);
U8G2_FOR_ADAFRUIT_GFX u8g2;

void powerOffNow() {         // 长按关机入口。电池模式=真关机；USB模式=深度待机
  Serial.println("[PWR] long press -> power off");
  display.setTextSize(2);
  display.setTextColor(TFT_RED);
  display.setTextDatum(MC_DATUM);
  display.fillScreen(TFT_BLACK);
  display.drawString("Powering off...", 240, 240);
  Serial.flush();
  delay(800);                                          // 让用户看清提示
  uint8_t out = ch32ReadReg(0x03);
  if (out == 0xFF) out = 0x3A;                         // 读不到就用已知的好值兜底
  ch32WriteReg(0x02, 0xFF);
  ch32WriteReg(0x03, out & (uint8_t)~0x20);            // bit5=SYS_EN 拉低 → 电池断电
  delay(1000);                                         // 给电池断电留时间
  // 走到这里说明是 USB 供电（还活着）→ 深度待机：屏幕熄灭，按 BAT-PWR 唤醒开机
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PWR_BTN_GPIO, 0);   // 低电平(按下)唤醒
  esp_deep_sleep_start();                              // 不会返回
}

// ===================== 通用小函数 =====================

void enterState(AppState s) {
  if (state == s) return;
  state = s;
  stateEnteredMs = millis();
  if (s == STATE_CLOCK) {
    lastClockMinute = -1;
    display.fillScreen(TFT_BLACK);
  } else if (s == STATE_COVER) {
    display.fillScreen(TFT_BLACK);
    prevKey = "";
  }
}

void showStatus(const char* msg, uint16_t color) {
  static String lastMsg; static uint16_t lastColor = 0;
  if (lastMsg == msg && lastColor == color) return;   // 内容没变就不重画，避免闪烁
  lastMsg = msg; lastColor = color;
  display.setTextSize(2);
  display.setTextColor(color);
  display.setTextDatum(BR_DATUM);
  display.fillRect(280, 420, 200, 60, TFT_BLACK);
  display.drawString(msg, 470, 470);
}

void showLogo() {
  uint8_t* logoBuf = (uint8_t*)ps_malloc(bootLogoLen);
  if (logoBuf) {
    memcpy_P(logoBuf, bootLogoData, bootLogoLen);
    display.drawJpg(logoBuf, bootLogoLen, LOGO_X, LOGO_Y, LOGO_W, LOGO_H, 0, 0, 0, 0);
    free(logoBuf);
  }
}

String urlEncode(const String& s) {
  String e; e.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') e += c;
    else if (c == ' ') e += "%20";
    else { char b[4]; snprintf(b, sizeof(b), "%%%02X", (unsigned char)c); e += b; }
  }
  return e;
}

// ===================== WiFi：非阻塞重连 =====================
// 原来的写法是 while(未连接) delay(3000) —— 程序会整段卡死。
// 现在每次 loop 调一次 wifiKick()：没连上就发起连接，15秒还没成
// 就重新发起。其余时间主循环照常跑，时钟照走、画面照刷。
void wifiKick() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiConnecting) {
      wifiConnecting = false;
      Serial.printf("[WiFi] connected, IP: %s\n", WiFi.localIP().toString().c_str());
    }
    return;
  }
  unsigned long now = millis();
  if (!wifiConnecting) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    wifiConnecting = true;
    wifiStartMs    = now;
    Serial.printf("[WiFi] connecting to %s ...\n", WIFI_SSID);
  } else if (now - wifiStartMs > (unsigned long)WIFI_REKICK_MS) {
    wifiConnecting = false;   // 15秒没连上，下一轮 loop 重新发起
    Serial.println("[WiFi] retry...");
  }
}

// ===================== 图片下载 =====================

void abortDownload() {
  if (dlHttp) { dlHttp->end(); delete dlHttp; dlHttp = nullptr; }
  if (dlBuf)  { free(dlBuf); dlBuf = nullptr; }
  dlStream   = nullptr;
  dlState    = DL_IDLE;
  dlSuccess  = false;
}

void startDownload(const String& url) {
  if (dlState != DL_IDLE) abortDownload();
  dlSuccess = false;
  dlHttp = new HTTPClient();
  dlHttp->begin(url);
  dlHttp->setConnectTimeout(8000);
  dlHttp->setTimeout(8000);
  int code = dlHttp->GET();
  if (code != 200) { Serial.printf("[DL] HTTP error %d\n", code); abortDownload(); return; }
  int len = dlHttp->getSize();
  dlSize = (len > 0) ? (size_t)len : (6UL * 1024 * 1024);
  if (dlSize > 6UL * 1024 * 1024) dlSize = 6UL * 1024 * 1024;   // 上限保护
  dlBuf  = (uint8_t*)ps_malloc(dlSize);
  if (!dlBuf) { Serial.println("[DL] ps_malloc failed"); abortDownload(); return; }
  dlStream     = dlHttp->getStreamPtr();
  dlPos        = 0;
  dlLastPos    = 0;
  dlStartMs    = millis();
  dlLastDataMs = dlStartMs;
  dlState      = DL_STREAM;
  Serial.printf("[DL] start, %d bytes\n", len);
}

bool tickDownload() {
  if (dlState == DL_IDLE) return false;
  unsigned long now = millis();
  if (now - dlStartMs > (unsigned long)DL_TOTAL_TIMEOUT_MS) {
    Serial.println("[DL] total timeout");
    abortDownload();
    return false;
  }

  if (dlState == DL_STREAM) {
    if (!dlStream) { abortDownload(); return false; }
    // v3: 按块读（一次最多4KB），比原来逐字节读快几十倍
    while (dlStream->available() && dlPos < dlSize) {
      size_t room = dlSize - dlPos;
      size_t want = (room < DL_CHUNK) ? room : DL_CHUNK;
      int got = dlStream->read(dlBuf + dlPos, want);
      if (got <= 0) break;
      dlPos        += (size_t)got;
      dlLastDataMs  = now;
    }
    // 卡死检测：8秒一个字节都没收到就放弃，让主循环去重试
    if (dlPos == dlLastPos) {
      if (now - dlLastDataMs > (unsigned long)DL_STALL_MS) {
        Serial.println("[DL] stall timeout");
        abortDownload();
        return false;
      }
    } else {
      dlLastPos = dlPos;
    }
    if (dlPos >= dlSize || (!dlStream->connected() && !dlStream->available())) {
      if (dlPos > 0) dlState = DL_DECODE;
      else { abortDownload(); return false; }
    }
  }

  if (dlState == DL_DECODE) {
    // v3: 校验 JPEG 文件头（FF D8），坏数据不画、按失败处理
    if (dlPos >= 1024 && dlBuf[0] == 0xFF && dlBuf[1] == 0xD8) {
      display.drawJpg(dlBuf, dlPos, 0, 0, 480, 480, 0, 0, 0, 0);
      dlSuccess      = true;
      lastServerOkMs = now;
      Serial.printf("[DL] ok, %u bytes\n", (unsigned)dlPos);
    } else {
      Serial.println("[DL] invalid JPEG data");
    }
    if (dlBuf)  { free(dlBuf); dlBuf = nullptr; }
    if (dlHttp) { dlHttp->end(); delete dlHttp; dlHttp = nullptr; }
    dlStream = nullptr;
    dlState  = DL_IDLE;
    return true;
  }
  return false;
}

// ===================== 服务器状态轮询 =====================

StatusInfo pollStatus() {
  StatusInfo info = {false, false, "", "", ""};
  static HTTPClient http;      // v3: 复用同一个客户端 = TCP长连接，开销小很多
  http.setReuse(true);
  String u = "http://" + String(SERVER_HOST) + ":" + SERVER_PORT + "/api/status";
  http.begin(u);
  http.setConnectTimeout(1500);
  http.setTimeout(1500);
  int code = http.GET();
  if (code != 200) { http.end(); return info; }
  String js = http.getString(); http.end();
  JsonDocument doc;
  if (deserializeJson(doc, js)) return info;
  info.ok        = true;
  info.isPlaying = doc["is_playing"] | false;
  const char* k  = doc["image_key"];
  if (k) info.imageKey = String(k);
  const char* t  = doc["three_line"]["line1"];
  if (t) info.title = String(t);
  const char* a  = doc["three_line"]["line3"];
  if (a) info.album = String(a);
  return info;
}

String buildImageUrl(const String& key, const String& album) {
  String u = "http://" + String(SERVER_HOST) + ":" + SERVER_PORT +
             "/roonapi/getImage?image_key=" + key;
  if (album.length()) u += "&albumName=" + urlEncode(album);
  return u;
}

// ===================== 时钟 =====================

void showClock() {
  time_t t = time(nullptr);
  if (t < 1700000000) {
    // 时间还没同步好（NTP没连上）。原来是直接黑屏，现在给出提示文字。
    display.setTextSize(2);
    display.setTextColor(0x8410);
    display.setTextDatum(MC_DATUM);
    display.drawString("Waiting for time sync...", 240, 240);
    return;
  }
  struct tm tm; localtime_r(&t, &tm);
  if (tm.tm_min == lastClockMinute) return;
  lastClockMinute = tm.tm_min;

  char buf[16];
  display.fillScreen(TFT_BLACK);
  strftime(buf, sizeof(buf), "%H:%M", &tm);
  display.setTextSize(8);
  display.setTextColor(TFT_WHITE);
  display.setTextDatum(MC_DATUM);
  display.drawString(buf, 240, 200);

  char dateStr[64];
  snprintf(dateStr, sizeof(dateStr), "%d年%d月%d日 %s",
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, WK_CN[tm.tm_wday]);
  u8g2.setFont(u8g2_font_wqy12_t_chinese3);
  u8g2.setForegroundColor(0xBDF7);
  uint16_t w = u8g2.getUTF8Width(dateStr);
  u8g2.setCursor((480 - w) / 2, 340);
  u8g2.print(dateStr);
}

// ===================== Setup =====================
void setup() {
  Serial.begin(115200); delay(300);
  Serial.println("\n[Boot] Roon Cover Display v3.1");

  tca9554_init();

  // 电源按键（GPIO16，按下=低电平，板上10K上拉）
  pinMode(PWR_BTN_GPIO, INPUT_PULLUP);
  delay(50);   // 上电瞬间电平会抖动，等它稳定
  if (digitalRead(PWR_BTN_GPIO) == LOW)
    Serial.println("[PWR] button held at boot (10s grace active)");
  else
    Serial.println("[PWR] button idle (GPIO16 high, OK)");

  // 电源系统诊断（只读探测，串口115200可看；用于确认协处理器在线）
  Serial.printf("[PWR] reg04(INPUT)=0x%02X reg07(RTC_INT)=0x%02X\n",
                ch32ReadReg(0x04), ch32ReadReg(0x07));
  for (uint8_t r = 0x08; r <= 0x0F; r++) {          // 未公开寄存器探测
    uint8_t v = ch32ReadReg(r);
    if (v != 0xFF) Serial.printf("[PWR] reg%02X=0x%02X\n", r, v);
  }
  float vb0 = ch32ReadBatteryV();
  if (vb0 > 0) Serial.printf("[PWR] battery %.2fV\n", vb0);

  display.init();
  display.setBrightness(255);
  u8g2.begin(gfxAdapter);
  u8g2.setFont(u8g2_font_wqy12_t_chinese3);

  showLogo();

  // v3: WiFi 全部改为非阻塞，开机不再等待卡在setup里。
  // 连WiFi、连服务器都在 loop 里自动进行，屏幕右下角会显示进度。
  WiFi.persistent(false);          // 不反复把WiFi密码写进flash
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);            // 关WiFi休眠，轮询更稳
  WiFi.setAutoReconnect(true);
  wifiKick();

  // v3: NTP 换成国内源。pool.ntp.org 在国内经常连不上 → 时钟永远出不来
  configTime(GMT_OFFSET_SEC, 0, NTP_SERVER, "cn.pool.ntp.org", "pool.ntp.org");

  lastServerOkMs = millis();
  state          = STATE_COVER;
  stateEnteredMs = millis();
  setupDoneMs    = millis();
  Serial.println("[Boot] done, entering loop");
}

// ===================== Loop =====================
void loop() {
  unsigned long now = millis();

  // 1) WiFi 非阻塞重连（原来的死等循环已删除）
  wifiKick();

  // 1.5) 电源按键：长按3秒关机。开机后前10秒忽略，防止唤醒时手没松被误关
  if (now - setupDoneMs > PWR_BOOT_GRACE_MS) {
    bool down = (digitalRead(PWR_BTN_GPIO) == LOW);
    if (down && !btnWasDown) { btnWasDown = true; btnDownSince = now; }
    else if (!down && btnWasDown) { btnWasDown = false; }      // 提前松开=短按，忽略
    else if (down && btnWasDown && now - btnDownSince >= PWR_OFF_HOLD_MS) {
      powerOffNow();                                           // 不会返回
    }
  }

  // 1.6) 电池电压日志（串口观察用，60秒一次，不影响画面）
  if (now - lastBattLogMs > BATT_LOG_MS) {
    lastBattLogMs = now;
    float vb = ch32ReadBatteryV();
    if (vb > 0) Serial.printf("[PWR] battery %.2fV\n", vb);
  }

  // 2) 自愈：连续10分钟完全联系不上服务器（含WiFi假死）→ 自动重启
  if (now - lastServerOkMs > (unsigned long)NO_CONTACT_RESTART_MS) {
    Serial.println("[Watchdog] no server contact, auto restart...");
    showStatus("REBOOT", TFT_RED);
    delay(2000);
    ESP.restart();
  }

  // 3) 图片下载推进（任何状态下都推进；原版在时钟状态下下载会卡住不前进）
  if (dlState != DL_IDLE) {
    bool finished = tickDownload();
    if (finished && !dlSuccess) {
      // v3: 下载失败 → 清掉记录，下一轮轮询会自动重试同一张封面
      prevKey   = "";
      prevTitle = "";
    }
    delay(2);
    return;
  }

  // 4) 时钟状态：每秒刷新一次
  if (state == STATE_CLOCK && now - lastClockTickMs > 1000) {
    lastClockTickMs = now;
    showClock();
  }

  // 5) 轮询节流
  if (now - lastPoll < (unsigned long)POLL_INTERVAL) { delay(5); return; }
  lastPoll = now;

  // 6) WiFi 未连接：只显示提示，绝不死等（v2的卡死源头）
  if (WiFi.status() != WL_CONNECTED) {
    if (!everPolled || state == STATE_CLOCK) showStatus("WiFi...", TFT_WHITE);
    return;
  }

  // 7) 问服务器当前播放状态
  StatusInfo info = pollStatus();
  if (!info.ok) {
    if (!everPolled || state == STATE_CLOCK) showStatus("Roon...", TFT_YELLOW);
    return;
  }
  if (!everPolled) {
    everPolled = true;
    Serial.println("[Roon] server reachable");
    display.fillRect(280, 420, 200, 60, TFT_BLACK);   // 清掉开机提示
  }
  lastServerOkMs = now;
  if (info.isPlaying) lastPlayingSeenMs = now;

  if (state == STATE_COVER) {
    bool keyChanged   = info.imageKey.length() && info.imageKey != prevKey;
    bool titleChanged = info.title.length()   && info.title   != prevTitle;
    if (keyChanged || titleChanged) {
      prevKey   = info.imageKey;
      prevTitle = info.title;
      startDownload(buildImageUrl(info.imageKey, info.album));
    }
    // 下载进行中不切时钟，避免图片刚画好就被清掉
    if (now - lastPlayingSeenMs > IDLE_TO_CLOCK_MS) {
      enterState(STATE_CLOCK);
    }
  } else {
    if (info.isPlaying) {
      enterState(STATE_COVER);
    } else {
      showClock();
    }
  }
}
