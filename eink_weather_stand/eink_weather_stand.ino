/*
  ESP32 E-Ink Weather + Calendar  (Jakarta, Indonesia)
  400x300 SSD1683 3-color (black/white/red) panel, Apple WeatherKit REST API

  WHAT IT SHOWS
    header    : date, city, time of last update
    now       : condition icon, temperature, feels-like, today's high/low, condition text
    calendar  : current month, today highlighted in red, Sundays in red
    details   : humidity, wind, gust, UV now/max, pressure + trend, visibility, dew point,
                cloud cover, rain chance, rain amount, sunrise, sunset
    bottom    : next 24 h every 3 hours (icon, temp, rain chance) - replaced by the
                active weather alert (from /api/v1/weatherAlert) when there is one
    footer    : tomorrow's summary, moon phase, data attribution

  HOW IT RUNS
    Wake -> WiFi -> NTP -> WeatherKit -> draw -> deep sleep until the next :00 / :30.
    The e-ink keeps the picture with no power. Press EN/RST on the ESP32 to refresh now.

  SETUP
    1. Generate a WeatherKit developer token (JWT) with your token generator page.
       Pick a long expiry (e.g. 180 days / 1 year); the footer warns 14 days before it expires.
    2. Fill in WIFI_SSID, WIFI_PASS and WEATHERKIT_TOKEN below.
    3. Libraries: GxEPD2 (ZinggJM), Adafruit GFX, ArduinoJson 7.x (Benoit Blanchon).
       WiFi / HTTPClient / WiFiClientSecure come with the ESP32 core.

  Wiring:
    Display: SDA->23  SCL->18  CS->5  D/C->17  RES->16  BUSY->4   VCC->3.3V  GND->GND
*/

#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "mbedtls/base64.h"
#include "esp_sleep.h"
#include <GxEPD2_3C.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

// ======================= YOUR SETTINGS =======================
const char* WIFI_SSID        = "99 Problems But Wi-Fi Aint One";
const char* WIFI_PASS        = "udinudinudin";
// Paste the generated JWT (the long "eyJ..." string). Do not share or commit it.
const char* WEATHERKIT_TOKEN = "eyJhbGciOiJFUzI1NiIsImtpZCI6IlpHMkM5UjVXQTkiLCJpZCI6IlNXUjgzRzdERE0uY29tLmtvZGludXMud2VhdGhlcmtpdC1jbGllbnQifQ.eyJpc3MiOiJTV1I4M0c3RERNIiwiaWF0IjoxNzkxOTg2MDk4LCJleHAiOjE4MjM1MjIwOTgsInN1YiI6ImNvbS5rb2RpbnVzLndlYXRoZXJraXQtY2xpZW50In0.MEYCIQCNC-9qY-N7tOGYmhFBFcyukc1GCllDmZHIRg1KJydWUgIhAK2-jM7Of6pvaVpOcw4onOZ4Smvjw3upGJ5YA7jb-mb9";

// ======================= CONFIG =======================
#define EPD_CS    5
#define EPD_DC    17
#define EPD_RST   16
#define EPD_BUSY  4
#define SPI_SCK   18
#define SPI_MISO  19
#define SPI_MOSI  23

#define CITY_NAME     "JAKARTA"
#define LATITUDE      "-6.2088"
#define LONGITUDE     "106.8456"
#define COUNTRY_CODE  "ID"              // needed for weather alerts
#define TIMEZONE_IANA "Asia/Jakarta"    // WeatherKit rolls daily forecasts up in this zone
#define TZ_POSIX      "WIB-7"           // UTC+7, no DST
#define LANGUAGE      "en"

const uint32_t UPDATE_MIN   = 30;       // refresh on every :00 and :30
const uint32_t RETRY_MIN    = 5;        // after a failed update
const uint8_t  FAILS_BEFORE_ERROR = 3;  // keep the old picture until this many failures in a row
const int      TOKEN_WARN_DAYS = 14;
const bool     WEEK_STARTS_MONDAY = false;   // Indonesian wall calendars start on Sunday (Minggu)

const char* NTP_1 = "id.pool.ntp.org";
const char* NTP_2 = "pool.ntp.org";
const char* NTP_3 = "time.google.com";

// weatherkit.apple.com chain: Apple Public Server ECC CA 1 - G1 -> DigiCert Global Root G3 (valid to 2038)
const char* ROOT_CA = R"PEM(
-----BEGIN CERTIFICATE-----
MIICPzCCAcWgAwIBAgIQBVVWvPJepDU1w6QP1atFcjAKBggqhkjOPQQDAzBhMQsw
CQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3d3cu
ZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBHMzAe
Fw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVTMRUw
EwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5jb20x
IDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEczMHYwEAYHKoZIzj0CAQYF
K4EEACIDYgAE3afZu4q4C/sLfyHS8L6+c/MzXRq8NOrexpu80JX28MzQC7phW1FG
fp4tn+6OYwwX7Adw9c+ELkCDnOg/QW07rdOkFFk2eJ0DQ+4QE2xy3q6Ip6FrtUPO
Z9wj/wMco+I+o0IwQDAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAd
BgNVHQ4EFgQUs9tIpPmhxdiuNkHMEWNpYim8S8YwCgYIKoZIzj0EAwMDaAAwZQIx
AK288mw/EkrRLTnDCgmXc/SINoyIJ7vmiI1Qhadj+Z4y3maTD/HMsQmP3Wyr+mt/
oAIwOWZbwmSNuJ5Q3KjVSaLtx9zRSX8XAbjIho9OjIgrqJqpisXRAL34VOKa5Vt8
sycX
-----END CERTIFICATE-----
)PEM";

GxEPD2_3C<GxEPD2_420c_GDEY042Z98, GxEPD2_420c_GDEY042Z98::HEIGHT> display(
  GxEPD2_420c_GDEY042Z98(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

#define BLACK GxEPD_BLACK
#define WHITE GxEPD_WHITE
#define RED   GxEPD_RED
#define DEG   "\xF8"                    // degree sign in the built-in font (cp437 mode)

// ======================= DATA =======================
enum Icon { IC_CLEAR, IC_PARTLY, IC_CLOUD, IC_RAIN, IC_THUNDER, IC_SNOW, IC_FOG, IC_WIND };

struct Current {
  bool ok = false;
  float temp = NAN, feels = NAN, humidity = NAN, cloud = NAN, precipRate = NAN;
  bool daylight = true;
  char cond[32] = "";
};

struct Day {
  bool ok = false;
  time_t start = 0, sunrise = 0, sunset = 0;
  float tMax = NAN, tMin = NAN, rainChance = NAN;
  char cond[32] = "", moon[20] = "", precipType[16] = "";
};

struct Hour {
  time_t t = 0;
  float temp = NAN, rainChance = NAN;
  bool daylight = true;
  char cond[32] = "";
};

struct Alert {
  bool active = false;
  char id[48] = "", desc[80] = "", severity[12] = "", urgency[12] = "", source[48] = "", area[48] = "";
  time_t effective = 0, expires = 0;
  char text[480] = "";
};

const int MAX_HOURS = 8;
Current cur;
Day today, tomorrow;
Hour hours[MAX_HOURS];
int hourCount = 0;
Alert alert;
time_t tokenExp = 0;

RTC_DATA_ATTR uint8_t failCount = 0;    // survives deep sleep
RTC_DATA_ATTR bool    everDrawn = false;

// ======================= TIME HELPERS =======================
// "2026-10-14T07:00:00Z" -> unix time (UTC)
time_t parseIso(const char* s) {
  int Y, M, D, h, m, sec;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  Y -= M <= 2;                                       // days-from-civil (H. Hinnant)
  long era = (Y >= 0 ? Y : Y - 399) / 400;
  unsigned yoe = (unsigned)(Y - era * 400);
  unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097 + (long)doe - 719468;
  return (time_t)days * 86400 + h * 3600 + m * 60 + sec;
}

time_t isoField(JsonVariantConst v) { return parseIso(v.as<const char*>()); }

String isoUtc(time_t t) {
  tm g;
  gmtime_r(&t, &g);
  char b[24];
  strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &g);
  return b;
}

String localFmt(time_t t, const char* fmt) {
  if (t <= 0) return "--";
  tm l;
  localtime_r(&t, &l);
  char b[48];
  strftime(b, sizeof(b), fmt, &l);
  return b;
}

time_t localMidnight(time_t t) {
  tm l;
  localtime_r(&t, &l);
  l.tm_hour = 0; l.tm_min = 0; l.tm_sec = 0;
  return mktime(&l);
}

bool timeValid() { return time(nullptr) > 1700000000; }

// Reads "exp" from the JWT payload so we can warn before the token runs out.
time_t readTokenExpiry() {
  String t = WEATHERKIT_TOKEN;
  int a = t.indexOf('.'), b = t.indexOf('.', a + 1);
  if (a < 0 || b < 0) return 0;
  String p = t.substring(a + 1, b);
  p.replace('-', '+');
  p.replace('_', '/');
  while (p.length() % 4) p += '=';
  static unsigned char out[512];
  size_t n = 0;
  if (mbedtls_base64_decode(out, sizeof(out) - 1, &n, (const unsigned char*)p.c_str(), p.length()) != 0) return 0;
  out[n] = 0;
  const char* e = strstr((const char*)out, "\"exp\":");
  return e ? (time_t)atoll(e + 6) : 0;
}

// ======================= TEXT HELPERS =======================
// "MostlyCloudy" -> "Mostly Cloudy", "waxingGibbous" -> "Waxing Gibbous"
void prettyCode(const char* code, char* out, size_t n) {
  size_t j = 0;
  for (size_t i = 0; code[i] && j + 2 < n; i++) {
    char c = code[i];
    if (i > 0 && isupper((unsigned char)c) && !isupper((unsigned char)code[i - 1])) out[j++] = ' ';
    out[j++] = (i == 0) ? toupper((unsigned char)c) : c;
  }
  out[j] = 0;
}

String pretty(const char* code) {
  char b[48];
  prettyCode(code, b, sizeof(b));
  return b;
}

int textWidth(const String& s, const GFXfont* f) {
  display.setFont(f);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(s.c_str(), 0, 100, &x1, &y1, &w, &h);
  return x1 + w;
}

String fitText(String s, const GFXfont* f, int maxW) {
  if (textWidth(s, f) <= maxW) return s;
  while (s.length() > 1 && textWidth(s + "..", f) > maxW) s.remove(s.length() - 1);
  s.trim();
  return s + "..";
}

int drawText(int x, int y, const String& s, const GFXfont* f, uint16_t color) {
  display.setFont(f);
  display.setTextColor(color);
  display.setCursor(x, y);
  display.print(s);
  return display.getCursorX();
}

// Built-in 6x8 font: (x, y) is the top-left corner.
void smallText(int x, int y, const String& s, uint16_t color) {
  display.setFont(nullptr);
  display.setTextSize(1);
  display.setTextColor(color);
  display.setCursor(x, y);
  display.print(s);
}

// Number followed by a drawn degree circle (the GFX fonts have no degree glyph).
int tempWidth(float t, const GFXfont* f, int r) {
  return textWidth(String((int)lroundf(t)), f) + 2 * r + 3;
}

int drawTemp(int x, int y, float t, const GFXfont* f, uint16_t color, int r) {
  if (isnan(t)) return drawText(x, y, "--", f, color);
  String s((int)lroundf(t));
  int end = drawText(x, y, s, f, color);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds("0", 0, y, &x1, &y1, &w, &h);
  int cx = end + r + 1, cy = y1 + r;
  display.drawCircle(cx, cy, r, color);
  if (r >= 3) display.drawCircle(cx, cy, r - 1, color);
  return cx + r + 2;
}

// ======================= ICONS =======================
Icon iconFor(const char* code) {
  String c = code;
  c.toLowerCase();
  auto has = [&](const char* k) { return c.indexOf(k) >= 0; };
  if (has("thunder") || has("storm") || has("hurricane")) return IC_THUNDER;
  if (has("snow") || has("flurr") || has("sleet") || has("blizzard") || has("hail") || has("wintry")) return IC_SNOW;
  if (has("rain") || has("drizzle") || has("shower")) return IC_RAIN;
  if (has("fog") || has("haze") || has("smok") || has("dust")) return IC_FOG;
  if (has("wind") || has("breez")) return IC_WIND;
  if (has("partly") || has("mostlyclear")) return IC_PARTLY;
  if (has("cloud")) return IC_CLOUD;
  return IC_CLEAR;
}

void thickLine(int x0, int y0, int x1, int y1, int t, uint16_t c) {
  for (int i = 0; i < t; i++) display.drawLine(x0 + i, y0, x1 + i, y1, c);
}

void drawSun(int cx, int cy, int r, int t, uint16_t c) {
  display.fillCircle(cx, cy, r, c);
  int gap = max(2, r / 3), len = max(3, r * 3 / 5);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    float ca = cosf(a), sa = sinf(a);
    int x0 = cx + ca * (r + gap), y0 = cy + sa * (r + gap);
    int x1 = cx + ca * (r + gap + len), y1 = cy + sa * (r + gap + len);
    display.drawLine(x0, y0, x1, y1, c);
    if (t > 1) {
      display.drawLine(x0 + 1, y0, x1 + 1, y1, c);
      display.drawLine(x0, y0 + 1, x1, y1 + 1, c);
    }
  }
}

void drawMoon(int cx, int cy, int r, uint16_t c) {
  display.fillCircle(cx, cy, r, c);
  display.fillCircle(cx + r * 55 / 100, cy - r * 35 / 100, r * 85 / 100, WHITE);
}

void cloudShape(int cx, int cy, int w, uint16_t c, int i) {
  display.fillCircle(cx - w * 25 / 100, cy + w * 5 / 100, w * 22 / 100 - i, c);
  display.fillCircle(cx + w * 2 / 100,  cy - w * 8 / 100, w * 30 / 100 - i, c);
  display.fillCircle(cx + w * 27 / 100, cy + w * 7 / 100, w * 19 / 100 - i, c);
  display.fillRoundRect(cx - w * 47 / 100 + i, cy, w * 93 / 100 - 2 * i, w * 27 / 100 - i,
                        max(1, w * 12 / 100 - i), c);
}

void drawCloud(int cx, int cy, int w, int t) {
  cloudShape(cx, cy, w, BLACK, 0);
  cloudShape(cx, cy, w, WHITE, t);
}

// Draws an icon centered at (cx, cy) inside an S x S box.
void drawIcon(Icon ic, bool day, int cx, int cy, int S) {
  int t = S >= 40 ? 2 : 1;
  switch (ic) {
    case IC_CLEAR:
      if (day) drawSun(cx, cy, S * 20 / 100, t, RED);
      else     drawMoon(cx, cy, S * 30 / 100, BLACK);
      return;
    case IC_PARTLY:
      if (day) drawSun(cx - S * 14 / 100, cy - S * 14 / 100, S * 15 / 100, t, RED);
      else     drawMoon(cx - S * 12 / 100, cy - S * 14 / 100, S * 22 / 100, BLACK);
      drawCloud(cx + S * 10 / 100, cy + S * 12 / 100, S * 72 / 100, t);
      return;
    case IC_CLOUD:
      drawCloud(cx, cy + S * 4 / 100, S * 92 / 100, t);
      return;
    case IC_WIND: {
      int r = max(2, S * 8 / 100);
      int ys[3] = { cy - S * 20 / 100, cy, cy + S * 20 / 100 };
      int ls[3] = { S * 60 / 100, S * 80 / 100, S * 50 / 100 };
      for (int k = 0; k < 3; k++) {
        int x0 = cx - S * 42 / 100, xe = x0 + ls[k];
        for (int i = 0; i < t; i++) display.drawFastHLine(x0, ys[k] + i, ls[k], BLACK);
        display.drawCircleHelper(xe, ys[k] - r, r, 0x2 | 0x4, BLACK);
      }
      return;
    }
    default: break;
  }

  // precipitation-type icons: cloud on top, something falling below
  drawCloud(cx, cy - S * 14 / 100, S * 86 / 100, t);
  int top = cy + S * 16 / 100, bot = cy + S * 46 / 100;
  if (ic == IC_RAIN) {
    for (int k = -1; k <= 1; k++) {
      int x = cx + k * S * 22 / 100;
      thickLine(x + S * 5 / 100, top, x - S * 5 / 100, bot, t, BLACK);
    }
  } else if (ic == IC_THUNDER) {
    display.fillTriangle(cx + S * 8 / 100, top - 1, cx - S * 12 / 100, cy + S * 32 / 100,
                         cx + S * 4 / 100, cy + S * 32 / 100, RED);
    display.fillTriangle(cx - S * 4 / 100, cy + S * 24 / 100, cx + S * 14 / 100, cy + S * 24 / 100,
                         cx - S * 8 / 100, cy + S * 50 / 100, RED);
    thickLine(cx - S * 28 / 100, top, cx - S * 33 / 100, bot - S * 8 / 100, t, BLACK);
    thickLine(cx + S * 30 / 100, top, cx + S * 25 / 100, bot - S * 8 / 100, t, BLACK);
  } else if (ic == IC_SNOW) {
    int a = max(2, S * 6 / 100);
    int pts[3][2] = { { cx - S * 22 / 100, top + a }, { cx, bot - a }, { cx + S * 22 / 100, top + a } };
    for (auto& p : pts) {
      display.drawLine(p[0] - a, p[1], p[0] + a, p[1], BLACK);
      display.drawLine(p[0], p[1] - a, p[0], p[1] + a, BLACK);
      display.drawLine(p[0] - a, p[1] - a, p[0] + a, p[1] + a, BLACK);
      display.drawLine(p[0] - a, p[1] + a, p[0] + a, p[1] - a, BLACK);
    }
  } else {                                           // fog
    int step = S * 12 / 100;
    for (int k = 0; k < 3; k++) {
      int w = S * (k == 1 ? 70 : 84) / 100;
      for (int i = 0; i < t; i++) display.drawFastHLine(cx - w / 2 + (k == 1 ? S / 10 : 0), top + k * step + i, w, BLACK);
    }
  }
}

// ======================= SCREENS =======================
void showMessage(const char* title, const String& l1, const String& l2, const String& l3) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(WHITE);
    display.fillRect(0, 0, display.width(), 26, RED);
    drawText(8, 19, title, &FreeSansBold9pt7b, WHITE);
    const String ls[3] = { l1, l2, l3 };
    for (int i = 0; i < 3; i++) drawText(8, 62 + i * 26, ls[i], &FreeSans9pt7b, BLACK);
    time_t now = time(nullptr);
    if (timeValid())
      smallText(8, 284, "Will retry automatically. Last attempt " + localFmt(now, "%d %b %H:%M"), BLACK);
  } while (display.nextPage());
}

void drawHeader(time_t now) {
  display.fillRect(0, 0, 400, 23, BLACK);
  drawText(6, 17, localFmt(now, "%A, %d %b %Y"), &FreeSansBold9pt7b, WHITE);
  String right = String(CITY_NAME) + "  " + localFmt(now, "%H:%M");
  drawText(394 - textWidth(right, &FreeSansBold9pt7b), 17, right, &FreeSansBold9pt7b, WHITE);
}

void drawNow() {
  drawIcon(iconFor(cur.cond), cur.daylight, 38, 60, 58);

  int x = drawTemp(74, 78, cur.temp, &FreeSansBold24pt7b, BLACK, 5);

  x = drawText(max(x + 10, 152), 66, "Feels ", &FreeSans9pt7b, BLACK);
  drawTemp(x, 66, cur.feels, &FreeSans9pt7b, BLACK, 2);

  drawText(8, 104, fitText(pretty(cur.cond), &FreeSansBold9pt7b, 238), &FreeSansBold9pt7b, BLACK);

  String line2;
  if (!isnan(cur.precipRate) && cur.precipRate >= 0.1f)
    line2 = "Raining now " + String(cur.precipRate, 1) + " mm/h";
  else if (today.ok)
    line2 = "Today: " + pretty(today.cond);
  if (line2.length() > 39) line2 = line2.substring(0, 38) + ".";
  smallText(8, 112, line2, BLACK);
}

void drawCalendar(int x0, int y0, time_t now) {
  const int CW = 20, RH = 12, W = CW * 7;
  tm t;
  localtime_r(&now, &t);

  String title = localFmt(now, "%B %Y");
  title.toUpperCase();
  smallText(x0 + (W - (int)title.length() * 6) / 2, y0, title, BLACK);

  const char* hdr = WEEK_STARTS_MONDAY ? "MTWTFSS" : "SMTWTFS";
  int sunCol = WEEK_STARTS_MONDAY ? 6 : 0;
  for (int c = 0; c < 7; c++)
    smallText(x0 + c * CW + (CW - 6) / 2, y0 + 11, String(hdr[c]), c == sunCol ? RED : BLACK);
  display.drawFastHLine(x0 + 2, y0 + 20, W - 4, BLACK);

  static const uint8_t mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  int year = t.tm_year + 1900;
  int dim = mdays[t.tm_mon] + (t.tm_mon == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0));
  int firstWday = (t.tm_wday - (t.tm_mday - 1) % 7 + 7) % 7;      // 0 = Sunday
  int offset = WEEK_STARTS_MONDAY ? (firstWday + 6) % 7 : firstWday;

  for (int d = 1; d <= dim; d++) {
    int cell = offset + d - 1, col = cell % 7, row = cell / 7;
    int x = x0 + col * CW, y = y0 + 24 + row * RH;
    String s(d);
    int tx = x + (CW - (int)s.length() * 6) / 2;
    if (d == t.tm_mday) {
      display.fillRect(x + 1, y - 2, CW - 2, RH - 1, RED);
      smallText(tx, y, s, WHITE);
    } else {
      smallText(tx, y, s, col == sunCol ? RED : BLACK);
    }
  }
}

// One cell of the details row: small label on top, big value below.
void detailCell(int col, const char* label, const String& value, uint16_t color = BLACK) {
  int x = col * 80 + 6, y = 131;
  smallText(x, y, label, BLACK);
  drawText(x, y + 26, value, &FreeSansBold12pt7b, color);
}

String pct(float v) {
  return isnan(v) ? String("--") : String(String((int)lroundf(v * 100)) + "%");
}

void drawDetails() {
  bool wet = !isnan(today.rainChance) && today.rainChance >= 0.5f;
  String rainLabel = (today.precipType[0] && strcmp(today.precipType, "clear") && strcmp(today.precipType, "precipitation"))
                     ? String(today.precipType) : String("rain");
  rainLabel += " chance";
  rainLabel.toUpperCase();

  detailCell(0, "HUMIDITY", pct(cur.humidity));
  detailCell(1, "CLOUD COVER", pct(cur.cloud));
  detailCell(2, rainLabel.c_str(), pct(today.rainChance), wet ? RED : BLACK);
  detailCell(3, "SUNRISE", localFmt(today.sunrise, "%H:%M"));
  detailCell(4, "SUNSET", localFmt(today.sunset, "%H:%M"));
}

void drawHourly() {
  if (hourCount == 0) {
    smallText(8, 220, "Hourly forecast not available.", BLACK);
    return;
  }
  for (int k = 0; k < hourCount; k++) {
    const Hour& h = hours[k];
    int x0 = k * 50, cx = x0 + 25;
    if (k > 0) display.drawFastVLine(x0, 168, 112, BLACK);
    smallText(cx - 15, 172, localFmt(h.t, "%H:%M"), BLACK);
    drawIcon(iconFor(h.cond), h.daylight, cx, 207, 38);
    drawTemp(cx - tempWidth(h.temp, &FreeSansBold12pt7b, 3) / 2, 255, h.temp, &FreeSansBold12pt7b, BLACK, 3);
    String p = pct(h.rainChance);
    smallText(cx - (int)p.length() * 3, 266, p, (!isnan(h.rainChance) && h.rainChance >= 0.5f) ? RED : BLACK);
  }
}

// Word-wraps text in the built-in font. Returns the number of lines used.
int wrapSmall(int x, int y, int maxChars, int maxLines, String s, uint16_t color) {
  s.replace("\r", " ");
  s.replace("\n", " ");
  s.trim();
  int line = 0;
  while (s.length() && line < maxLines) {
    String part;
    if ((int)s.length() <= maxChars) {
      part = s;
      s = "";
    } else {
      int cut = s.lastIndexOf(' ', maxChars);
      if (cut <= 0) cut = maxChars;
      part = s.substring(0, cut);
      s = s.substring(cut);
      s.trim();
      if (line == maxLines - 1) part = part.substring(0, maxChars - 2) + "..";
    }
    smallText(x, y + line * 10, part, color);
    line++;
  }
  return line;
}

void drawAlert() {
  display.fillRect(0, 165, 400, 18, RED);
  String sev = alert.severity;
  sev.toUpperCase();
  String title = (sev.length() ? String(sev + ": ") : String("ALERT: ")) + alert.desc;
  drawText(6, 179, fitText(title, &FreeSansBold9pt7b, 388), &FreeSansBold9pt7b, WHITE);
  display.drawRect(0, 165, 400, 118, RED);

  String when = localFmt(alert.effective, "%d %b %H:%M") + " - " + localFmt(alert.expires, "%d %b %H:%M");
  if (alert.source[0]) when += "  " + String(alert.source);
  smallText(6, 188, when.substring(0, 64), RED);

  String body = alert.text[0] ? String(alert.text) : String(alert.area);
  if (alert.urgency[0] && !alert.text[0]) body += "  (urgency: " + String(alert.urgency) + ")";
  wrapSmall(6, 201, 64, 8, body, BLACK);
}

void drawFooter(time_t now) {
  if (tomorrow.ok) {
    char b[48];
    snprintf(b, sizeof(b), "Tmrw %d-%d" DEG " %d%% ", (int)lroundf(tomorrow.tMin), (int)lroundf(tomorrow.tMax),
             isnan(tomorrow.rainChance) ? 0 : (int)lroundf(tomorrow.rainChance * 100));
    String s = String(b) + pretty(tomorrow.cond);
    if (s.length() > 30) s = s.substring(0, 29) + ".";
    smallText(4, 289, s, BLACK);
  }

  long daysLeft = tokenExp ? (long)((tokenExp - now) / 86400) : 9999;
  if (daysLeft < TOKEN_WARN_DAYS) {
    smallText(192, 289, "TOKEN EXPIRES " + String(daysLeft) + "D", RED);
  } else if (today.moon[0]) {
    smallText(192, 289, pretty(today.moon), BLACK);
  }

  smallText(396 - 13 * 6, 289, "Apple Weather", BLACK);
}

void drawDashboard() {
  time_t now = time(nullptr);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(WHITE);
    drawHeader(now);
    drawNow();
    display.drawFastVLine(252, 28, 96, BLACK);
    drawCalendar(256, 28, now);
    display.drawFastHLine(0, 127, 400, BLACK);
    drawDetails();
    display.drawFastHLine(0, 163, 400, BLACK);
    if (alert.active) drawAlert();
    else drawHourly();
    display.drawFastHLine(0, 285, 400, BLACK);
    drawFooter(now);
  } while (display.nextPage());
}

// ======================= NETWORK =======================
bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
  if (WiFi.status() != WL_CONNECTED) return false;
  Serial.printf("WiFi OK, IP %s, RSSI %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

bool syncTime() {
  configTzTime(TZ_POSIX, NTP_1, NTP_2, NTP_3);
  uint32_t t0 = millis();
  while (!timeValid() && millis() - t0 < 15000) delay(200);
  return timeValid();
}

// GET a WeatherKit URL and parse the JSON (keeping only the fields in `filter`).
// Returns the HTTP status, or a negative value on connection / parse errors.
int weatherKitGet(const String& url, JsonDocument& doc, JsonDocument& filter) {
  WiFiClientSecure client;
  client.setCACert(ROOT_CA);
  HTTPClient http;
  http.useHTTP10(true);                              // no chunked encoding -> stream straight into ArduinoJson
  http.setTimeout(15000);
  if (!http.begin(client, url)) return -1;
  http.addHeader("Authorization", String("Bearer ") + WEATHERKIT_TOKEN);
  int code = http.GET();
  if (code == 200) {
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    if (err) {
      Serial.printf("JSON error: %s\n", err.c_str());
      code = -2;
    }
  } else {
    Serial.printf("HTTP %d for %s\n", code, url.c_str());
  }
  http.end();
  return code;
}

int severityRank(const char* s) {
  if (!strcmp(s, "extreme"))  return 4;
  if (!strcmp(s, "severe"))   return 3;
  if (!strcmp(s, "moderate")) return 2;
  if (!strcmp(s, "minor"))    return 1;
  return 0;
}

void fetchAlertDetail() {
  JsonDocument filter;
  filter["messages"][0]["text"] = true;
  filter["areaName"] = true;
  JsonDocument doc;
  String url = String("https://weatherkit.apple.com/api/v1/weatherAlert/" LANGUAGE "/") + alert.id;
  if (weatherKitGet(url, doc, filter) != 200) return;
  strlcpy(alert.text, doc["messages"][0]["text"] | "", sizeof(alert.text));
  if (!alert.area[0]) strlcpy(alert.area, doc["areaName"] | "", sizeof(alert.area));
}

int fetchWeather() {
  time_t now = time(nullptr);
  time_t midnight = localMidnight(now);
  time_t hourStart = now - now % 3600;

  String url = "https://weatherkit.apple.com/api/v1/weather/" LANGUAGE "/" LATITUDE "/" LONGITUDE
               "?countryCode=" COUNTRY_CODE "&timezone=" TIMEZONE_IANA
               "&dataSets=currentWeather,forecastDaily,forecastHourly,weatherAlerts"
               "&dailyStart=" + isoUtc(midnight) + "&dailyEnd=" + isoUtc(midnight + 2 * 86400) +
               "&hourlyStart=" + isoUtc(hourStart) + "&hourlyEnd=" + isoUtc(hourStart + 26 * 3600);

  JsonDocument filter;
  JsonObject fc = filter["currentWeather"].to<JsonObject>();
  for (const char* k : { "conditionCode", "daylight", "temperature", "temperatureApparent", "humidity",
                         "cloudCover", "precipitationIntensity" })
    fc[k] = true;
  JsonObject fd = filter["forecastDaily"]["days"][0].to<JsonObject>();
  for (const char* k : { "forecastStart", "forecastEnd", "conditionCode", "temperatureMax", "temperatureMin",
                         "precipitationChance", "precipitationType", "sunrise", "sunset", "moonPhase" })
    fd[k] = true;
  JsonObject fh = filter["forecastHourly"]["hours"][0].to<JsonObject>();
  for (const char* k : { "forecastStart", "conditionCode", "temperature", "precipitationChance", "daylight" })
    fh[k] = true;
  JsonObject fa = filter["weatherAlerts"]["alerts"][0].to<JsonObject>();
  for (const char* k : { "id", "description", "severity", "urgency", "source", "areaName",
                         "effectiveTime", "expireTime" })
    fa[k] = true;

  JsonDocument doc;
  int code = weatherKitGet(url, doc, filter);
  if (code != 200) return code;

  // ---- current
  JsonObject c = doc["currentWeather"];
  cur = Current();
  cur.ok = !c.isNull();
  strlcpy(cur.cond, c["conditionCode"] | "", sizeof(cur.cond));
  cur.daylight   = c["daylight"] | true;
  cur.temp       = c["temperature"] | NAN;
  cur.feels      = c["temperatureApparent"] | NAN;
  cur.humidity   = c["humidity"] | NAN;
  cur.cloud      = c["cloudCover"] | NAN;
  cur.precipRate = c["precipitationIntensity"] | NAN;
  if (!cur.ok) return -3;

  // ---- daily: find the day that contains "now", the next one is tomorrow
  JsonArray days = doc["forecastDaily"]["days"];
  int todayIdx = 0;
  for (int i = 0; i < (int)days.size(); i++) {
    time_t s = isoField(days[i]["forecastStart"]), e = isoField(days[i]["forecastEnd"]);
    if (s <= now && now < e) { todayIdx = i; break; }
  }
  auto readDay = [&](JsonObject d, Day& out) {
    out = Day();
    if (d.isNull()) return;
    out.ok         = true;
    out.start      = isoField(d["forecastStart"]);
    out.sunrise    = isoField(d["sunrise"]);
    out.sunset     = isoField(d["sunset"]);
    out.tMax       = d["temperatureMax"] | NAN;
    out.tMin       = d["temperatureMin"] | NAN;
    out.rainChance = d["precipitationChance"] | NAN;
    strlcpy(out.cond, d["conditionCode"] | "", sizeof(out.cond));
    strlcpy(out.moon, d["moonPhase"] | "", sizeof(out.moon));
    strlcpy(out.precipType, d["precipitationType"] | "", sizeof(out.precipType));
  };
  readDay(days[todayIdx].as<JsonObject>(), today);
  readDay(days[todayIdx + 1].as<JsonObject>(), tomorrow);

  // ---- hourly: the next 8 hours that fall on 00/03/06/.../21 local time
  hourCount = 0;
  for (JsonObject h : doc["forecastHourly"]["hours"].as<JsonArray>()) {
    if (hourCount >= MAX_HOURS) break;
    time_t t = isoField(h["forecastStart"]);
    tm l;
    localtime_r(&t, &l);
    if (t <= now || l.tm_hour % 3 != 0) continue;
    Hour& o = hours[hourCount++];
    o.t = t;
    o.temp = h["temperature"] | NAN;
    o.rainChance = h["precipitationChance"] | NAN;
    o.daylight = h["daylight"] | true;
    strlcpy(o.cond, h["conditionCode"] | "", sizeof(o.cond));
  }

  // ---- alerts: keep the most severe one that is still in effect
  alert = Alert();
  int best = -1;
  for (JsonObject a : doc["weatherAlerts"]["alerts"].as<JsonArray>()) {
    time_t exp = isoField(a["expireTime"]);
    if (exp && exp < now) continue;
    int r = severityRank(a["severity"] | "");
    if (r <= best) continue;
    best = r;
    alert.active = true;
    strlcpy(alert.id, a["id"] | "", sizeof(alert.id));
    strlcpy(alert.desc, a["description"] | "Weather alert", sizeof(alert.desc));
    strlcpy(alert.severity, a["severity"] | "", sizeof(alert.severity));
    strlcpy(alert.urgency, a["urgency"] | "", sizeof(alert.urgency));
    strlcpy(alert.source, a["source"] | "", sizeof(alert.source));
    strlcpy(alert.area, a["areaName"] | "", sizeof(alert.area));
    alert.effective = isoField(a["effectiveTime"]);
    alert.expires = exp;
  }
  if (alert.active && alert.id[0]) fetchAlertDetail();

  Serial.printf("Now %s %.1fC feels %.1fC hum %.0f%% | today %s %.0f-%.0fC rain %.0f%% | %d hours | alert: %s\n",
                cur.cond, cur.temp, cur.feels, cur.humidity * 100, today.cond, today.tMin, today.tMax,
                today.rainChance * 100, hourCount, alert.active ? alert.desc : "none");
  return 200;
}

// ======================= SLEEP =======================
void sleepFor(uint32_t sec) {
  Serial.printf("Sleeping %lu s\n", (unsigned long)sec);
  Serial.flush();
  display.hibernate();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup((uint64_t)sec * 1000000ULL);
  esp_deep_sleep_start();
}

uint32_t secondsToNextUpdate() {
  if (!timeValid()) return RETRY_MIN * 60;
  uint32_t period = UPDATE_MIN * 60;
  time_t now = time(nullptr);
  return period - (uint32_t)(now % period) + 15;     // a few seconds past :00 / :30 so the date flips on time
}

// Counts a failed update; draws the error only once it is not a one-off glitch.
void failed(const char* title, const String& l1, const String& l2, const String& l3, bool showNow = false) {
  Serial.printf("%s: %s %s\n", title, l1.c_str(), l2.c_str());
  if (failCount < 255) failCount++;
  if (showNow || !everDrawn || failCount >= FAILS_BEFORE_ERROR) showMessage(title, l1, l2, l3);
  sleepFor(RETRY_MIN * 60);
}

// ======================= SETUP / LOOP =======================
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(EPD_CS, OUTPUT);
  digitalWrite(EPD_CS, HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, EPD_CS);
  display.init(115200);
  display.setRotation(0);
  display.setTextWrap(false);
  display.cp437(true);

  if (strstr(WEATHERKIT_TOKEN, "PASTE_YOUR") || strstr(WIFI_SSID, "YOUR_WIFI")) {
    showMessage("SETUP NEEDED", "Edit the sketch and fill in:", "WIFI_SSID, WIFI_PASS and", "WEATHERKIT_TOKEN, then upload.");
    sleepFor(24UL * 3600UL);
  }

  if (!connectWiFi())
    failed("WIFI ERROR", "Cannot join \"" + String(WIFI_SSID) + "\".", "Check the name / password and", "that it is a 2.4 GHz network.");

  if (!syncTime())
    failed("TIME ERROR", "Could not get the time from NTP.", "Internet may be down.", "");

  time_t now = time(nullptr);
  tokenExp = readTokenExpiry();
  if (tokenExp && tokenExp <= now) {
    showMessage("TOKEN EXPIRED", "WeatherKit token expired on", localFmt(tokenExp, "%d %b %Y %H:%M") + ".",
                "Generate a new one and re-upload.");
    sleepFor(6UL * 3600UL);
  }

  int code = fetchWeather();
  if (code == 401 || code == 403)
    failed("TOKEN REJECTED", "WeatherKit answered HTTP " + String(code) + ".", "Check Team ID / Key ID / Service ID",
           "and generate a new token.", true);
  if (code != 200)
    failed("WEATHER ERROR", code > 0 ? "WeatherKit answered HTTP " + String(code) + "."
                                     : "Request failed (" + String(code) + ").",
           "Will retry in " + String(RETRY_MIN) + " minutes.", "");

  drawDashboard();
  failCount = 0;
  everDrawn = true;
  sleepFor(secondsToNextUpdate());
}

void loop() {
  // never reached: setup() always ends in deep sleep
}
