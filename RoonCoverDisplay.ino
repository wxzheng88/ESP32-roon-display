/**
 * Roon Cover Display v2
 * =====================
 * Hardware: Waveshare ESP32-S3-Touch-LCD-4 V4 (non-touch)
 * Display:  ST7701, 480x480, RGB parallel
 * Features:
 *   - Full-screen cover art during playback
 *   - NTP clock (UTC+8) shown 15s after playback stops
 *   - Non-blocking image download — reacts instantly to track changes
 *   - Chinese date / weekday via U8g2 CJK font
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <time.h>
#include <U8g2_for_Adafruit_GFX.h>

#include "boot_logo.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

#include "wifi_config.h"

// ===================== Configuration =====================
const int  POLL_INTERVAL       = 200;
const int  WIFI_RETRY_MS       = 3000;
const int  IDLE_TO_CLOCK_MS    = 15000;
const long DL_TOTAL_TIMEOUT_MS = 30000;
const long GMT_OFFSET_SEC      = 8 * 3600;
const char* NTP_SERVER        = "pool.ntp.org";

// ===================== Forward declarations =====================
enum AppState { STATE_COVER, STATE_CLOCK };
struct StatusInfo { bool ok; bool isPlaying; String imageKey; String title; String album; };
void enterState(AppState s);
void showStatus(const char* msg, uint16_t color);
void showLogo();
String urlEncode(const String& s);
bool downloadImage(const String& url);
void abortDownload();
void startDownload(const String& url);
bool tickDownload();
StatusInfo pollStatus();
String buildImageUrl(const String& key, const String& album);
void showClock();

// ===================== Globals =====================
AppState          state              = STATE_COVER;
unsigned long     stateEnteredMs     = 0;
unsigned long     lastPlayingSeenMs  = 0;
unsigned long     lastClockTickMs    = 0;
int               lastClockMinute    = -1;
String            prevKey            = "";
String            prevTitle          = "";
unsigned long     lastPoll           = 0;

enum DlState { DL_IDLE, DL_STREAM, DL_DECODE };
DlState           dlState     = DL_IDLE;
HTTPClient*       dlHttp      = nullptr;
WiFiClient*       dlStream    = nullptr;
uint8_t*          dlBuf       = nullptr;
size_t            dlPos       = 0;
size_t            dlSize      = 0;
unsigned long     dlStartMs   = 0;

const char* WK_CN[] = {"周日","周一","周二","周三","周四","周五","周六"};

// ===================== TCA9554 =====================
void tca9554_init() {
  Wire.begin(15, 7); Wire.setClock(400000);
  Wire.beginTransmission(0x24); Wire.write(0x02); Wire.write(0xff); Wire.endTransmission();
  Wire.beginTransmission(0x24); Wire.write(0x03); Wire.write(0x3a); Wire.endTransmission();
}

// ===================== Display =====================
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

// ===================== Implementations =====================

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

void abortDownload() {
  if (dlHttp) { dlHttp->end(); delete dlHttp; dlHttp = nullptr; }
  if (dlBuf)  { free(dlBuf); dlBuf = nullptr; }
  dlStream  = nullptr;
  dlState   = DL_IDLE;
}

void startDownload(const String& url) {
  if (dlState != DL_IDLE) abortDownload();
  dlHttp = new HTTPClient();
  dlHttp->begin(url);
  dlHttp->setTimeout(15000);
  if (dlHttp->GET() != 200) { abortDownload(); return; }
  int len = dlHttp->getSize();
  dlSize = (len > 0) ? (size_t)len : (6UL * 1024 * 1024);
  dlBuf  = (uint8_t*)ps_malloc(dlSize);
  if (!dlBuf) { abortDownload(); return; }
  dlStream  = dlHttp->getStreamPtr();
  dlPos     = 0;
  dlStartMs = millis();
  dlState   = DL_STREAM;
}

bool tickDownload() {
  if (dlState == DL_IDLE) return false;
  if (millis() - dlStartMs > DL_TOTAL_TIMEOUT_MS) { abortDownload(); return false; }
  if (dlState == DL_STREAM) {
    if (!dlStream) { abortDownload(); return false; }
    while (dlStream->available() && dlPos < dlSize) {
      int c = dlStream->read();
      if (c >= 0) dlBuf[dlPos++] = (uint8_t)c;
    }
    if (!dlStream->connected() && !dlStream->available()) {
      if (dlPos > 0) { dlState = DL_DECODE; }
      else { abortDownload(); return false; }
    }
  }
  if (dlState == DL_DECODE) {
    display.drawJpg(dlBuf, dlPos, 0, 0, 480, 480, 0, 0, 0, 0);
    free(dlBuf); dlBuf = nullptr;
    if (dlHttp) { dlHttp->end(); delete dlHttp; dlHttp = nullptr; }
    dlState = DL_IDLE;
    return true;
  }
  return false;
}

StatusInfo pollStatus() {
  StatusInfo info = {false, false, "", "", ""};
  HTTPClient http;
  String u = "http://" + String(SERVER_HOST) + ":" + SERVER_PORT + "/api/status";
  http.begin(u); http.setTimeout(5000);
  if (http.GET() != 200) { http.end(); return info; }
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

void showClock() {
  time_t t = time(nullptr);
  if (t < 1700000000) return;
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
  Serial.begin(115200); delay(500);
  tca9554_init();
  display.init();
  display.setBrightness(255);
  u8g2.begin(gfxAdapter);
  u8g2.setFont(u8g2_font_wqy12_t_chinese3);

  showLogo();

  showStatus("WiFi...", TFT_WHITE);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) delay(1000);
  if (WiFi.status() == WL_CONNECTED) showStatus("WiFi OK", TFT_GREEN);

  configTime(GMT_OFFSET_SEC, 0, NTP_SERVER);

  showStatus("Roon...", TFT_YELLOW);
  int retry = 0;
  while (true) {
    StatusInfo info = pollStatus();
    if (info.ok && info.imageKey.length()) {
      prevKey           = info.imageKey;
      prevTitle         = info.title;
      lastPlayingSeenMs = millis();
      startDownload(buildImageUrl(info.imageKey, info.album));
      unsigned long t0 = millis();
      while (dlState != DL_IDLE && millis() - t0 < 30000) {
        tickDownload();
        delay(10);
      }
      if (dlState == DL_IDLE) { retry = 0; break; }
    }
    delay(2000);
    retry++;
    if (retry >= 3) { showStatus("Offline", TFT_RED); while (true) delay(1000); }
  }

  state          = STATE_COVER;
  stateEnteredMs = millis();
}

// ===================== Loop =====================
void loop() {
  unsigned long now = millis();

  if (state == STATE_COVER) {
    tickDownload();
  } else {
    if (now - lastClockTickMs > 1000) {
      lastClockTickMs = now;
      showClock();
    }
  }

  if (now - lastPoll < POLL_INTERVAL) { delay(10); return; }
  lastPoll = now;

  if (WiFi.status() != WL_CONNECTED) {
    showStatus("WiFi...", TFT_WHITE);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) delay(WIFI_RETRY_MS);
    return;
  }

  StatusInfo info = pollStatus();
  if (!info.ok) return;
  if (info.isPlaying) lastPlayingSeenMs = now;

  if (state == STATE_COVER) {
    bool keyChanged   = info.imageKey.length() && info.imageKey != prevKey;
    bool titleChanged = info.title.length()   && info.title   != prevTitle;
    if (keyChanged || titleChanged) {
      prevKey   = info.imageKey;
      prevTitle = info.title;
      startDownload(buildImageUrl(info.imageKey, info.album));
    }
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
