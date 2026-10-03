// CYD-Weather: shows `curl wttr.in` style ASCII weather on the Cheap Yellow Display.
//
// The ESP32 requests http://wttr.in/<location>?0 with a curl User-Agent, so wttr.in
// replies with the same ANSI-coloured terminal text you'd see in a shell. We parse
// the ANSI colour codes + UTF-8, then draw it on the 320x240 TFT, re-laid out so the
// ASCII art icon and readings are big enough to read.
//
// Controls: tap the screen = next location (or refresh if only one), BOOT button = refresh.

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <vector>
#include <esp_system.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.example.h to include/secrets.h and fill in your WiFi details"
#endif

// CYD touch controller lives on its own SPI bus
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33
#define BOOT_BUTTON 0

static const int SCREEN_W = 320;
static const int SCREEN_H = 240;
static const int FOOTER_Y = 231;
static const size_t ART_TEXT_COL = 16;  // wttr.in ?0 output: art in cols 0-15, text from col 16

TFT_eSPI tft;
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

static const uint16_t BG = TFT_BLACK;
static uint16_t DEFAULT_FG, BOLD_FG, HEADER_BG, DIM_FG;

static const size_t NUM_LOCATIONS = sizeof(WTTR_LOCATIONS) / sizeof(WTTR_LOCATIONS[0]);
static size_t locationIndex = 0;
static uint32_t lastFetchMs = 0;
static uint32_t lastSuccessMs = 0;
static bool haveData = false;
static uint32_t lastFooterMs = 0;
static uint32_t lastStatusLogMs = 0;
static String lastFetchResult = "none yet";

// ---------------------------------------------------------------------------
// ANSI + UTF-8 parsing
// ---------------------------------------------------------------------------

struct Cell {
  uint32_t cp;  // unicode codepoint
  uint16_t fg;  // RGB565
};
using Line = std::vector<Cell>;

static uint16_t xterm256(int n) {
  static const uint8_t base[16][3] = {
      {0, 0, 0},       {205, 0, 0},   {0, 205, 0},   {205, 205, 0},
      {0, 0, 238},     {205, 0, 205}, {0, 205, 205}, {229, 229, 229},
      {127, 127, 127}, {255, 0, 0},   {0, 255, 0},   {255, 255, 0},
      {92, 92, 255},   {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
  static const uint8_t level[6] = {0, 95, 135, 175, 215, 255};
  n = constrain(n, 0, 255);
  if (n < 16) return tft.color565(base[n][0], base[n][1], base[n][2]);
  if (n < 232) {
    n -= 16;
    return tft.color565(level[n / 36], level[(n / 6) % 6], level[n % 6]);
  }
  uint8_t v = 8 + (n - 232) * 10;
  return tft.color565(v, v, v);
}

struct AnsiState {
  int color = -1;  // -1 = default, otherwise RGB565
  bool bold = false;

  uint16_t fg() const {
    if (color >= 0) return (uint16_t)color;
    return bold ? BOLD_FG : DEFAULT_FG;
  }

  void applySgr(const int *p, int np) {
    for (int k = 0; k < np; k++) {
      int v = p[k];
      if (v == 0) { color = -1; bold = false; }
      else if (v == 1) bold = true;
      else if (v == 22) bold = false;
      else if (v >= 30 && v <= 37) color = xterm256(v - 30 + (bold ? 8 : 0));
      else if (v >= 90 && v <= 97) color = xterm256(v - 90 + 8);
      else if (v == 39) color = -1;
      else if (v == 38 && k + 2 < np && p[k + 1] == 5) { color = xterm256(p[k + 2]); k += 2; }
      else if (v == 38 && k + 4 < np && p[k + 1] == 2) {
        color = tft.color565(p[k + 2], p[k + 3], p[k + 4]);
        k += 4;
      }
    }
  }
};

static std::vector<Line> parseAnsi(const String &text) {
  std::vector<Line> lines(1);
  AnsiState st;
  const uint8_t *p = (const uint8_t *)text.c_str();
  size_t n = text.length(), i = 0;

  while (i < n) {
    uint8_t c = p[i];

    if (c == 0x1B && i + 1 < n && p[i + 1] == '[') {  // CSI sequence
      int params[16], np = 0, cur = -1;
      size_t j = i + 2;
      while (j < n && !(p[j] >= 0x40 && p[j] <= 0x7E)) {
        if (isdigit(p[j])) cur = (cur < 0 ? 0 : cur * 10) + (p[j] - '0');
        else if (p[j] == ';' && np < 16) { params[np++] = cur < 0 ? 0 : cur; cur = -1; }
        j++;
      }
      if (np < 16) params[np++] = cur < 0 ? 0 : cur;
      if (j < n && p[j] == 'm') st.applySgr(params, np);
      i = j + 1;
      continue;
    }
    if (c == '\n') { lines.emplace_back(); i++; continue; }
    if (c == '\r') { i++; continue; }
    if (c == '\t') {
      do lines.back().push_back({' ', st.fg()}); while (lines.back().size() % 8);
      i++;
      continue;
    }

    uint32_t cp;
    int len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else { cp = '?'; len = 1; }
    for (int k = 1; k < len; k++) {
      if (i + k >= n || (p[i + k] & 0xC0) != 0x80) { cp = '?'; len = k; break; }
      cp = (cp << 6) | (p[i + k] & 0x3F);
    }
    if (cp >= 0x20) lines.back().push_back({cp, st.fg()});
    i += len;
  }

  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

// ---------------------------------------------------------------------------
// Glyph rendering. Plain ASCII uses the built-in 5x7 font; the handful of Unicode
// symbols wttr.in uses (arrows, degree, rain ticks, lightning...) are drawn by hand.
// ---------------------------------------------------------------------------

static char foldToAscii(uint32_t cp) {
  if (cp >= 0x20 && cp < 0x7F) return (char)cp;
  if (cp >= 0xC0 && cp <= 0xFF) {
    static const char latin1[] =
        "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"
        "aaaaaaaceeeeiiiidnooooo/ouuuuypy";
    return latin1[cp - 0xC0];
  }
  switch (cp) {
    case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x201C: case 0x201D: return '"';
    case 0x2026: return '.';
    case 0x00A0: return ' ';
    default: return '?';
  }
}

static void thickLine(int x0, int y0, int x1, int y1, int s, uint16_t c) {
  for (int t = 0; t < s; t++) {
    tft.drawLine(x0 + t, y0, x1 + t, y1, c);
    if (s > 1) tft.drawLine(x0, y0 + t, x1, y1 + t, c);
  }
}

static void drawArrow(int x, int y, int s, int dx, int dy, uint16_t c) {
  // glyph box is 5s x 7s inside a 6s x 8s cell
  int cx = x + (5 * s) / 2, cy = y + (7 * s) / 2;
  bool diag = dx && dy;
  int r = diag ? (5 * s) / 2 : 3 * s;
  int tipX = cx + dx * r, tipY = cy + dy * r;
  thickLine(cx - dx * r, cy - dy * r, tipX, tipY, (s + 1) / 2, c);
  // arrowhead: tip + h * (back +/- perpendicular)
  int h = diag ? s + 1 : 2 * s;
  int bx = -dx, by = -dy, px = -dy, py = dx;
  if (diag) {
    thickLine(tipX, tipY, tipX + (bx + px) * h / 2, tipY + (by + py) * h / 2, (s + 1) / 2, c);
    thickLine(tipX, tipY, tipX + (bx - px) * h / 2, tipY + (by - py) * h / 2, (s + 1) / 2, c);
  } else {
    thickLine(tipX, tipY, tipX + (bx + px) * h, tipY + (by + py) * h, (s + 1) / 2, c);
    thickLine(tipX, tipY, tipX + (bx - px) * h, tipY + (by - py) * h, (s + 1) / 2, c);
  }
}

static void drawGlyph(int x, int y, uint32_t cp, uint16_t fg, int s) {
  const int w = 6 * s, h = 8 * s;
  switch (cp) {
    case ' ':
    case 0x00A0:
      return;
    case 0x00B0:  // °
      tft.drawCircle(x + 2 * s, y + 2 * s, s + (s > 1 ? 0 : 1), fg);
      if (s > 2) tft.drawCircle(x + 2 * s, y + 2 * s, s - 1, fg);
      return;
    case 0x2190: drawArrow(x, y, s, -1, 0, fg); return;   // ←
    case 0x2191: drawArrow(x, y, s, 0, -1, fg); return;   // ↑
    case 0x2192: drawArrow(x, y, s, 1, 0, fg); return;    // →
    case 0x2193: drawArrow(x, y, s, 0, 1, fg); return;    // ↓
    case 0x2196: drawArrow(x, y, s, -1, -1, fg); return;  // ↖
    case 0x2197: drawArrow(x, y, s, 1, -1, fg); return;   // ↗
    case 0x2198: drawArrow(x, y, s, 1, 1, fg); return;    // ↘
    case 0x2199: drawArrow(x, y, s, -1, 1, fg); return;   // ↙
    case 0x2015: case 0x2500: case 0x2014:  // ― full-width dash (sun rays)
      tft.fillRect(x, y + 3 * s, w, s, fg);
      return;
    case 0x203E:  // ‾ overline (fog)
      tft.fillRect(x, y, w, s, fg);
      return;
    case 0x2018: case 0x2019: case 0x02BB: case 0x0027:  // ‘ ’ ʻ ' rain ticks
      thickLine(x + 3 * s, y + s, x + 2 * s, y + 4 * s, s, fg);
      return;
    case 0x201A:  // ‚ low tick
      thickLine(x + 3 * s, y + 4 * s, x + 2 * s, y + 7 * s, s, fg);
      return;
    case 0x26A1: {  // ⚡
      int t = (s + 1) / 2;
      thickLine(x + 4 * s, y, x + s, y + 4 * s, t, fg);
      thickLine(x + s, y + 4 * s, x + 4 * s, y + 4 * s, t, fg);
      thickLine(x + 4 * s, y + 4 * s, x + s, y + h - 1, t, fg);
      return;
    }
  }
  tft.drawChar(x, y, foldToAscii(cp), fg, BG, s);
}

// Draw cells [from, to) of a line starting at pixel (x, y)
static void drawCells(const Line &line, size_t from, size_t to, int x, int y, int s) {
  to = min(to, line.size());
  for (size_t i = from; i < to; i++, x += 6 * s) {
    if (x + 6 * s > SCREEN_W) break;
    drawGlyph(x, y, line[i].cp, line[i].fg, s);
  }
}

static void drawCentered(const Line &line, int y, int s) {
  int maxCols = SCREEN_W / (6 * s);
  size_t len = min(line.size(), (size_t)maxCols);
  int x = (SCREEN_W - (int)len * 6 * s) / 2;
  drawCells(line, 0, len, x, y, s);
}

// Slice [from, to) out of a line with leading/trailing spaces trimmed
static Line trimmed(const Line &line, size_t from, size_t to = SIZE_MAX) {
  to = min(to, line.size());
  while (from < to && line[from].cp == ' ') from++;
  while (to > from && line[to - 1].cp == ' ') to--;
  return Line(line.begin() + from, line.begin() + to);
}

static Line plainLine(const char *text, uint16_t fg) {
  Line l;
  for (const char *c = text; *c; c++) l.push_back({(uint8_t)*c, fg});
  return l;
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

// Re-send the panel's init sequence. If the Wi-Fi radio's current spike makes the
// supply dip, the display controller can reset to its power-on state (solid white)
// while the ESP32 keeps running; this brings it back.
static void reinitDisplay() {
  tft.init();
  tft.setRotation(1);  // landscape; use 3 to flip
}

static void drawStatus(const char *title, const String &detail) {
  tft.fillScreen(BG);
  drawCentered(plainLine(title, TFT_YELLOW), 90, 2);
  drawCentered(plainLine(detail.c_str(), DIM_FG), 125, 1);
}

static void drawFooter() {
  tft.fillRect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, BG);
  char buf[48];
  if (haveData) {
    uint32_t mins = (millis() - lastSuccessMs) / 60000;
    if (mins == 0) snprintf(buf, sizeof(buf), "updated just now");
    else snprintf(buf, sizeof(buf), "updated %lu min ago", (unsigned long)mins);
    drawCells(plainLine(buf, DIM_FG), 0, SIZE_MAX, 4, FOOTER_Y + 1, 1);
  }
  if (NUM_LOCATIONS > 1) {
    snprintf(buf, sizeof(buf), "%u/%u", (unsigned)locationIndex + 1, (unsigned)NUM_LOCATIONS);
    drawCells(plainLine(buf, DIM_FG), 0, SIZE_MAX, SCREEN_W - 4 - 6 * strlen(buf), FOOTER_Y + 1, 1);
  }
  lastFooterMs = millis();
}

// Anything that isn't the normal ?0 layout (errors, rate-limit notices...) is
// shown verbatim, terminal-style, in the small font with word-wrapping.
static void drawRaw(const std::vector<Line> &lines) {
  reinitDisplay();
  tft.fillScreen(BG);
  const int cols = SCREEN_W / 6, maxRows = FOOTER_Y / 8;
  int row = 0;
  for (const Line &l : lines) {
    size_t pos = 0;
    do {
      if (row >= maxRows) return;
      drawCells(l, pos, pos + cols, 0, row++ * 8, 1);
      pos += cols;
    } while (pos < l.size());
  }
}

static void drawWeather(const std::vector<Line> &lines) {
  // Expected shape of `curl wttr.in/X?0`:
  //   0: Weather report: X
  //   1: (blank)
  //   2-6: <16 cols of ASCII art><condition | temp | wind | visibility | precipitation>
  static const char *kHeader = "Weather report:";
  bool ok = lines.size() >= 7 && lines[0].size() > strlen(kHeader);
  for (size_t i = 0; ok && i < strlen(kHeader); i++) ok = lines[0][i].cp == (uint8_t)kHeader[i];
  if (!ok) {
    Serial.println("[draw] reply is not the usual weather layout, showing it as raw text");
    drawRaw(lines);
    return;
  }

  Serial.println("[draw] drawing weather screen");
  reinitDisplay();
  tft.fillScreen(BG);

  // Header bar with the location name
  tft.fillRect(0, 0, SCREEN_W, 22, HEADER_BG);
  Line loc = trimmed(lines[0], strlen(kHeader));
  for (Cell &c : loc) c.fg = TFT_WHITE;
  if (loc.empty()) loc = plainLine("My location", TFT_WHITE);
  int maxCols = SCREEN_W / 12;
  if ((int)loc.size() > maxCols) {
    loc.resize(maxCols - 1);
    loc.push_back({'.', TFT_WHITE});
  }
  int lx = (SCREEN_W - (int)loc.size() * 12) / 2;
  for (size_t i = 0; i < loc.size(); i++) drawGlyph(lx + i * 12, 3, loc[i].cp, loc[i].fg, 2);

  // ASCII art icon at 2x, trimmed of common blank margins and centered
  size_t artFrom = ART_TEXT_COL, artTo = 0;
  for (int r = 2; r <= 6; r++) {
    const Line &l = lines[r];
    size_t end = min(l.size(), ART_TEXT_COL);
    for (size_t c = 0; c < end; c++) {
      if (l[c].cp != ' ') { artFrom = min(artFrom, c); artTo = max(artTo, c + 1); }
    }
  }
  if (artTo > artFrom) {
    int ax = (SCREEN_W - (int)(artTo - artFrom) * 12) / 2;
    for (int r = 2; r <= 6; r++) drawCells(lines[r], artFrom, artTo, ax, 28 + (r - 2) * 16, 2);
  }

  // Readings
  Line cond = trimmed(lines[2], ART_TEXT_COL);
  Line temp = trimmed(lines[3], ART_TEXT_COL);
  Line wind = trimmed(lines[4], ART_TEXT_COL);
  Line vis = trimmed(lines[5], ART_TEXT_COL);
  Line rain = trimmed(lines[6], ART_TEXT_COL);

  int y = 114;
  // condition, word-wrapped to at most two lines of the 2x font
  const size_t condCols = SCREEN_W / 12;
  size_t pos = 0;
  for (int pass = 0; pass < 2 && pos < cond.size(); pass++) {
    size_t end = min(cond.size(), pos + condCols);
    if (end < cond.size() && pass == 0) {
      size_t sp = end;
      while (sp > pos && cond[sp].cp != ' ') sp--;
      if (sp > pos) end = sp;
    }
    drawCentered(trimmed(cond, pos, end), y, 2);
    y += 20;
    pos = end;
  }

  y += 2;
  drawCentered(temp, y, temp.size() * 18 <= (size_t)SCREEN_W ? 3 : 2);
  y += 30;
  drawCentered(wind, y, 2);
  y += 22;

  Line visRain = vis;
  for (int i = 0; i < 3; i++) visRain.push_back({' ', DEFAULT_FG});
  visRain.insert(visRain.end(), rain.begin(), rain.end());
  drawCentered(visRain, y, 2);
}

// ---------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------

static String urlEncode(const char *s) {
  String out;
  for (; *s; s++) {
    char c = *s;
    if (isalnum((uint8_t)c) || strchr("-_.~,@", c)) out += c;
    else if (c == ' ') out += '+';
    else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", (uint8_t)c);
      out += hex;
    }
  }
  return out;
}

static void retryInOneMinute() {
  lastFetchMs = millis() - REFRESH_MINUTES * 60000UL + 60000UL;
}

static const char *wifiStatusName(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS: return "idle";
    case WL_NO_SSID_AVAIL: return "SSID not found";
    case WL_SCAN_COMPLETED: return "scan completed";
    case WL_CONNECTED: return "connected";
    case WL_CONNECT_FAILED: return "connect failed (wrong password?)";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED: return "disconnected";
    default: return "unknown";
  }
}

static const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT (power supply dipped)";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_EXT: return "reset pin";
    default: return "other";
  }
}

// Periodic one-line status so you can see the board is alive
static void logStatus() {
  uint32_t sinceFetch = (millis() - lastFetchMs) / 1000;
  uint32_t period = REFRESH_MINUTES * 60UL;
  uint32_t nextIn = period > sinceFetch ? period - sinceFetch : 0;
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[status] uptime %lus | WiFi connected, IP %s, RSSI %d dBm | heap %u | last fetch: %s | next in %lus\n",
                  millis() / 1000, WiFi.localIP().toString().c_str(), WiFi.RSSI(), ESP.getFreeHeap(),
                  lastFetchResult.c_str(), (unsigned long)nextIn);
  } else {
    Serial.printf("[status] uptime %lus | WiFi %s | heap %u | last fetch: %s | next in %lus\n",
                  millis() / 1000, wifiStatusName(WiFi.status()), ESP.getFreeHeap(),
                  lastFetchResult.c_str(), (unsigned long)nextIn);
  }
  lastStatusLogMs = millis();
}

static bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  drawStatus("Connecting WiFi", WIFI_SSID);
  Serial.printf("[wifi] connecting to \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[wifi] FAILED after 20s: %s, retrying in 1 minute\n", wifiStatusName(WiFi.status()));
    lastFetchResult = "WiFi failed";
    drawStatus("WiFi failed", "retrying in 1 minute");
    return false;
  }
  Serial.printf("[wifi] connected in %lums\n", millis() - start);
  Serial.printf("[wifi]   IP address: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[wifi]   gateway:    %s\n", WiFi.gatewayIP().toString().c_str());
  Serial.printf("[wifi]   DNS:        %s\n", WiFi.dnsIP().toString().c_str());
  Serial.printf("[wifi]   RSSI:       %d dBm, channel %d\n", WiFi.RSSI(), WiFi.channel());
  Serial.println("[display] re-initialising panel after WiFi start");
  reinitDisplay();
  tft.fillScreen(BG);
  return true;
}

static void fetchAndShow() {
  lastFetchMs = millis();
  if (!ensureWifi()) { retryInOneMinute(); return; }

  String url = "http://wttr.in/" + urlEncode(WTTR_LOCATIONS[locationIndex]) + "?0" WTTR_UNITS;
  Serial.printf("[fetch] location %u/%u: \"%s\"\n", (unsigned)locationIndex + 1, (unsigned)NUM_LOCATIONS,
                WTTR_LOCATIONS[locationIndex][0] ? WTTR_LOCATIONS[locationIndex] : "(auto from IP)");
  Serial.printf("[fetch] GET %s\n", url.c_str());

  // Small "fetching" marker in the footer so a tap gives instant feedback
  tft.fillRect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, BG);
  drawCells(plainLine("fetching...", TFT_YELLOW), 0, SIZE_MAX, 4, FOOTER_Y + 1, 1);

  uint32_t t0 = millis();
  HTTPClient http;
  http.setUserAgent("curl/8.5.0");  // makes wttr.in return terminal text, not HTML
  http.setTimeout(15000);
  http.begin(url);
  int code = http.GET();
  String body = code > 0 ? http.getString() : String();
  http.end();

  Serial.printf("[fetch] HTTP %d (%s), %u bytes in %lums, free heap %u\n", code,
                code > 0 ? "ok" : HTTPClient::errorToString(code).c_str(), body.length(), millis() - t0,
                ESP.getFreeHeap());

  if (code <= 0) {
    lastFetchResult = "FAILED: " + HTTPClient::errorToString(code);
    Serial.println("[fetch] request failed, retrying in 1 minute");
    if (haveData) drawFooter();  // keep showing the last good data
    else drawStatus("Fetch failed", HTTPClient::errorToString(code));
    retryInOneMinute();
    return;
  }

  std::vector<Line> lines = parseAnsi(body);
  if (lines.empty()) lines.push_back(plainLine(("HTTP " + String(code) + ", empty reply").c_str(), DEFAULT_FG));

  // Echo the reply without colour codes, the way the screen will interpret it
  Serial.printf("[fetch] reply (%u lines):\n", (unsigned)lines.size());
  for (const Line &l : lines) {
    String text = "  | ";
    for (const Cell &c : l) {
      char ch = foldToAscii(c.cp);
      text += (ch == '?' && c.cp != '?') ? '*' : ch;  // * = symbol drawn by hand
    }
    Serial.println(text);
  }

  lastFetchResult = code == 200 ? String("OK") : "HTTP " + String(code);
  drawWeather(lines);
  haveData = true;
  lastSuccessMs = millis();
  drawFooter();
  Serial.printf("[fetch] done, next refresh in %d min\n", REFRESH_MINUTES);
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== CYD-Weather starting ===");
  Serial.printf("[boot] reset reason: %s\n", resetReasonName(esp_reset_reason()));
  Serial.printf("[boot] free heap %u, %u location(s), units \"%s\", refresh every %d min\n",
                ESP.getFreeHeap(), (unsigned)NUM_LOCATIONS, WTTR_UNITS, REFRESH_MINUTES);
  pinMode(BOOT_BUTTON, INPUT_PULLUP);

  reinitDisplay();
  Serial.println("[display] initialised");
  DEFAULT_FG = xterm256(7);
  BOLD_FG = TFT_WHITE;
  HEADER_BG = tft.color565(20, 40, 90);
  DIM_FG = xterm256(244);
  tft.fillScreen(BG);

  touchSpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSpi);
  ts.setRotation(1);

  fetchAndShow();
}

void loop() {
  bool tapped = ts.tirqTouched() && ts.touched();
  bool boot = digitalRead(BOOT_BUTTON) == LOW;

  if (tapped || boot) {
    Serial.println(tapped ? "[input] screen tapped" : "[input] BOOT button pressed");
    if (tapped && NUM_LOCATIONS > 1) locationIndex = (locationIndex + 1) % NUM_LOCATIONS;
    fetchAndShow();
    // wait for release so one tap = one action
    while (ts.touched() || digitalRead(BOOT_BUTTON) == LOW) delay(20);
    delay(150);
  }

  if (millis() - lastFetchMs >= REFRESH_MINUTES * 60000UL) fetchAndShow();
  if (haveData && millis() - lastFooterMs >= 60000) drawFooter();
  if (millis() - lastStatusLogMs >= 30000) logStatus();

  delay(20);
}
