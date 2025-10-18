/*
ESP32 SimHub Dash for ILI9341 (320x240) + 8x WS2812 LEDs
Updates:
- Top-left box shows lap counter as "cur/total" using fixed fonts (no label).
- If TotalLaps is 0 or not provided, a graphical infinity symbol is drawn to the right of the slash.
- Uses dedicated fixed-font constants for the lap number so it's easy to tweak.
- GEAR uses a fixed font size (no autosizing), with a margin.
- Margin is applied around the gear text so it never kisses the frame.
- Removed all autosizing/font-scaling logic; fonts are fixed per area.
- CSV parsing extended to optionally accept two additional trailing fields:
    field 14 (index 13) = CurrentLap (GameDataCurrentLap)
    field 15 (index 14) = TotalLaps  (GameData.TotalLaps)
*/

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <NeoPixelBus.h>
#include <WiFi.h>
#include <esp_wifi.h>
#ifdef ARDUINO_ARCH_ESP32
  #include <esp_bt.h>
#endif

// Fonts
#include <Fonts/FreeSansBold66pt7b.h>
#include <Fonts/FreeSansBold48pt7b.h>
#include <Fonts/FreeSansBold36pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>

#include <limits.h>

// ================== HARDWARE PINS ==================
#define TFT_CS   27
#define TFT_DC   16
#define TFT_RST   4

// WS2812 strip (8 LEDs by default)
#define LED_PIN   13
#define LED_COUNT 8

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
NeoPixelBus<NeoGrbFeature, NeoEsp32Rmt0800KbpsMethod> strip(LED_COUNT, LED_PIN);

// ================== LED BRIGHTNESS ==================
const float LED_BRIGHTNESS = 0.07f;
static inline RgbColor dimColor(RgbColor c) {
  return RgbColor(
    (uint8_t)(c.R * LED_BRIGHTNESS),
    (uint8_t)(c.G * LED_BRIGHTNESS),
    (uint8_t)(c.B * LED_BRIGHTNESS)
  );
}

// ================== COLORS ==================
const uint16_t C_BG  = ILI9341_BLACK;
const uint16_t C_TX  = ILI9341_WHITE;
const uint16_t C_BOX = ILI9341_YELLOW;
const uint16_t C_OK  = ILI9341_GREEN;
const uint16_t C_BAD = ILI9341_RED;

// ================== STATE ==================
int rpm=0, maxRpm=8000, speed=0, gear=0, pos=0, fuel=0;
long lapMs=0, bestMs=0, deltaMs=0;
int flagYellow=0, flagBlue=0, flagRed=0, flagGreen=0;

// New lap counters (from SimHub)
int curLap = 0;
int totLaps = 0;

// ================== CHANGE-TRACKING ==================
int crpm=-1, cspeed=-1, cgear=-999, cpos=-1, cfuel=-1;
long clap=-1, cbest=-1, cdelta=LONG_MIN/2;

// change tracking for laps
int ccurLap = -1, cTotLaps = -1;

// ================== DATA FRESHNESS ==================
unsigned long lastDataTime = 0;
bool noDataScreenActive = false;

// ================== LAYOUT ==================
const int M  = 6;
const int G  = 8;
const int BW = 92;
const int BH = 56;

const int Y1 = M + 6;
const int Y2 = Y1 + BH + G;

const int LX  = M;
const int RXc = 320 - M - BW;

const int BOT_H = 60;
const int BOT_Y = 240 - M - BOT_H;

const int GX = LX + BW + G;
const int GW = 320 - (2*M + 2*BW + 2*G);
const int GY = Y1 - 6;
const int GH = (BOT_Y - G) - GY;

const int BBW = (320 - (3*M))/2;
const int BLX = M;
const int BRX = BLX + BBW + M;

static inline int BH_speedFuel() { return (GY + GH) - Y2; }

// ================== FIXED FONTS ==================
// Choose one fixed font per logical area (no autosizing)
static const GFXfont* FONT_GEAR  = &FreeSansBold66pt7b;
static const GFXfont* FONT_SPEED = &FreeSansBold24pt7b;
static const GFXfont* FONT_RPM   = &FreeSansBold12pt7b;
static const GFXfont* FONT_POS   = &FreeSansBold24pt7b;
static const GFXfont* FONT_FUEL  = &FreeSansBold18pt7b;
static const GFXfont* FONT_LAPBEST = &FreeSansBold12pt7b;
static const GFXfont* FONT_DELTA = &FreeSansBold18pt7b;

// Dedicated lap counter fonts (easy to tweak independently)
static const GFXfont* FONT_LAP_NUMBER = &FreeSansBold18pt7b; // larger current/total

// ================== HELPERS ==================
static inline String msToStr(long ms) {
  bool neg = ms < 0; if (neg) ms = -ms;
  long t = ms/1000, mm = t/60, ss = t%60, sss = ms%1000;
  char b[20];
  snprintf(b, sizeof(b), "%s%ld:%02ld.%03ld", neg?"-":"", mm, ss, sss);
  return String(b);
}

void frame(int x,int y,int w,int h){ tft.drawRect(x,y,w,h,C_BOX); }

// Center a single-string using a fixed font
static void drawCenteredText_fixedFont(int x,int y,int w,int h,const GFXfont* font,const String& s,uint16_t col){
  tft.fillRect(x+2,y+2,w-4,h-4,C_BG);
  tft.setFont(font);
  tft.setTextSize(1);
  int16_t bx,by; uint16_t bw,bh;
  tft.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  int cx = x + (w - bw)/2 - bx;
  int cy = y + (h - bh)/2 - by;
  tft.setTextColor(col);
  tft.setCursor(cx, cy);
  tft.print(s);
  tft.setFont(); // restore classic
}

// Two-line stacked text with fixed fonts (topFont for top, botFont for bottom)
static void drawTwoLineCentered_fixedFonts(int x,int y,int w,int h,const GFXfont* topFont,const String& top,const GFXfont* botFont,const String& bottom,uint16_t col){
  const int gap = 6;
  tft.fillRect(x+2,y+2,w-4,h-4,C_BG);
  tft.setTextWrap(false);

  // measure top
  tft.setFont(topFont); tft.setTextSize(1);
  int16_t tbx,tby; uint16_t tbw,tbh;
  tft.getTextBounds(top, 0, 0, &tbx, &tby, &tbw, &tbh);

  // measure bottom
  tft.setFont(botFont); tft.setTextSize(1);
  int16_t bbx,bby; uint16_t bbw,bbh;
  tft.getTextBounds(bottom, 0, 0, &bbx, &bby, &bbw, &bbh);

  int totalH = (int)tbh + gap + (int)bbh;
  int blockTop = y + (h - totalH)/2;

  int topBaseline = blockTop - tby;
  int botBaseline = blockTop + tbh + gap - bby;

  int cxTop = x + (w - tbw)/2 - tbx;
  int cxBot = x + (w - bbw)/2 - bbx;

  tft.setTextColor(col);
  tft.setFont(topFont); tft.setCursor(cxTop, topBaseline); tft.print(top);
  tft.setFont(botFont); tft.setCursor(cxBot, botBaseline); tft.print(bottom);

  tft.setFont(); // restore classic
}

// ================== STATIC BACKGROUND ==================
void drawStatic(){
  tft.fillScreen(C_BG);

  // Left column
  frame(LX,  Y1, BW, BH);              // top-left: will show lap counter
  frame(LX,  Y2, BW, BH_speedFuel());  // speed+rpm live here

  // Right column
  frame(RXc, Y1, BW, BH);              // position box
  frame(RXc, Y2, BW, BH_speedFuel());  // fuel box
  tft.setFont(); tft.setTextSize(2); tft.setTextColor(C_TX);
  tft.setCursor(RXc+6, Y2+6); tft.print("fuel");

  // Center gear box
  frame(GX,  GY, GW, GH);

  // Bottom row
  frame(BLX, BOT_Y, BBW, BOT_H);       // lap/best
  frame(BRX, BOT_Y, BBW, BOT_H);       // delta
}

// ================== NO DATA ==================
void drawNoDataScreen() {
  tft.fillScreen(C_BG);
  tft.setFont(); tft.setTextSize(3); tft.setTextColor(C_BAD);
  int16_t x1, y1; uint16_t w, h;
  String msg = "NO DATA FEED";
  tft.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
  int x = (320 - w) / 2; int y = (240 - h) / 2;
  tft.setCursor(x, y); tft.print(msg);
}

void setNoDataLeds() {
  static unsigned long lastBlink=0;
  static bool on=false, lastOn=false;
  unsigned long now=millis();
  if (now - lastBlink >= 1000) { lastBlink = now; on = !on; }
  if (on != lastOn) {
    for (int i = 0; i < LED_COUNT; i++) {
      RgbColor c = ((i == 0 || i == LED_COUNT-1) && on) ? dimColor(RgbColor(255,0,0))
                                                        : dimColor(RgbColor(0));
      strip.SetPixelColor(i, c);
    }
    strip.Show();
    lastOn = on;
  }
}

void clearAllLeds() {
  for (int i = 0; i < LED_COUNT; i++) strip.SetPixelColor(i, dimColor(RgbColor(0)));
  strip.Show();
}

// ================== DYNAMIC DRAW ==================
void drawSpeedRpmStack(){
  if (speed == cspeed && rpm == crpm) return;
  // fixed fonts: speed top, rpm bottom
  drawTwoLineCentered_fixedFonts(LX, Y2, BW, BH_speedFuel(), FONT_SPEED, String(speed), FONT_RPM, String(rpm), C_TX);
  cspeed = speed; crpm = rpm;
}

void drawFuel(){
  if(fuel==cfuel) return;
  tft.fillRect(RXc+2, Y2+2, BW-4, BH_speedFuel()-4, C_BG);

  String s = String(fuel) + "%";
  tft.setFont(FONT_FUEL); tft.setTextSize(1);
  int16_t bx, by; uint16_t bw, bh;
  tft.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  int cx = RXc + (BW - bw)/2 - bx;
  int cy = Y2  + (BH_speedFuel() - bh)/2 - by;
  tft.setTextColor(C_TX);
  tft.setCursor(cx, cy); tft.print(s);
  tft.setFont(); // classic
  cfuel=fuel;
}

// ========== GEAR FIXED FONT WITH MARGIN ==========
void drawGear(){
  if (gear == cgear) return;
  String g = (gear<0) ? "R" : (gear==0) ? "N" : String(gear);

  // Clear box interior
  tft.fillRect(GX+2, GY+2, GW-4, GH-4, C_BG);

  // Fixed font for gear
  const GFXfont* chosen = FONT_GEAR;

  // Margin inside the box so the glyphs don't touch the frame
  const int pad = 6;  // tweak if you want more/less air
  const int maxW = GW - 2*pad;
  const int maxH = GH - 2*pad;

  tft.setTextWrap(false);
  tft.setFont(chosen);
  tft.setTextSize(1);

  int16_t bx, by; uint16_t bw, bh;
  tft.getTextBounds(g, 0, 0, &bx, &by, &bw, &bh);

  // Note: we no longer autosize. If the chosen font doesn't fit, it may clip.
  int cx = GX + (GW - bw)/2 - bx;
  int cy = GY + (GH - bh)/2 - by;
  tft.setTextColor(C_BOX);
  tft.setCursor(cx, cy);
  tft.print(g);

  // Restore classic
  tft.setFont(); tft.setTextSize(1);

  cgear = gear;
}

void drawPosBig(){
  if(pos==cpos) return;
  String s = String("P") + String(pos);
  drawCenteredText_fixedFont(RXc+2, Y1+2, BW-4, BH-4, FONT_POS, s, C_TX);
  cpos=pos;
}

void drawLapBest() {
  bool hasLast = lapMs > 0;
  bool hasBest = bestMs > 0;
  if (lapMs == clap && bestMs == cbest) return;

  int x0 = BLX + 4, y0 = BOT_Y + 3, w = BBW - 8, h = BOT_H - 6;
  tft.fillRect(x0, y0, w, h, C_BG);

  tft.setFont(); tft.setTextColor(C_TX); tft.setTextSize(1);
  const int labelOffsetX = 6;
  const int labelOffsetY = 5;
  tft.setCursor(x0 + labelOffsetX, y0 + labelOffsetY + 8);     tft.print("last:");
  tft.setCursor(x0 + labelOffsetX, y0 + h/2 + labelOffsetY);   tft.print("best:");

  String sLast = hasLast ? msToStr(lapMs) : String("--:--.---");
  String sBest = hasBest ? msToStr(bestMs) : String("--:--.---");
  tft.setFont(FONT_LAPBEST); tft.setTextSize(1);

  int16_t bx, by; uint16_t bw, bh;
  int rightEdge = x0 + w - 6;

  tft.getTextBounds(sLast, 0, 0, &bx, &by, &bw, &bh);
  int baseY1 = y0 + (h / 4) + (bh / 2) - 2;
  tft.setCursor(rightEdge - bw, baseY1); tft.print(sLast);

  tft.getTextBounds(sBest, 0, 0, &bx, &by, &bw, &bh);
  int baseY2 = y0 + (3 * h / 4) + (bh / 2) - 1;
  tft.setCursor(rightEdge - bw, baseY2); tft.print(sBest);

  tft.setFont(); tft.setTextSize(1);
  clap = lapMs; cbest = bestMs;
}

static const int DELTA_BASELINE_FIX = 0;
void drawDelta() {
  if (deltaMs == cdelta) return;

  const int x0 = BRX + 4, y0 = BOT_Y + 3, w = BBW - 8, h = BOT_H - 6;
  tft.fillRect(x0, y0, w, h, C_BG);

  long v = deltaMs; unsigned long a = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
  int whole = a / 1000; int frac = a % 1000; if (whole > 99) { whole = 99; frac = 999; }
  char sign = (v < 0 ? '-' : (v > 0 ? '+' : '±'));

  char buf[12];
  if (whole < 10) snprintf(buf, sizeof(buf), "%c %d.%03d", sign, whole, frac);
  else snprintf(buf, sizeof(buf), "%c%02d.%03d", sign, whole, frac);

  uint16_t col = (v < 0) ? C_OK : (v > 0 ? C_BAD : C_TX);

  tft.setTextSize(1);
  tft.setFont(FONT_DELTA);
  tft.setTextColor(col);

  int16_t bx, by; uint16_t bw, bh;
  tft.getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
  int cx = x0 + (w - bw) / 2 - bx;
  int cy = y0 + (h - bh) / 2 - by;
  tft.setCursor(cx, cy); tft.print(buf);

  tft.setFont(); tft.setTextSize(1);
  cdelta = deltaMs;
}

// ================== LAP COUNTER (TOP-LEFT BOX) ==================
void drawLapCounter() {
  // only redraw when values change
  if (curLap == ccurLap && totLaps == cTotLaps) return;

  // clear box interior
  tft.fillRect(LX+2, Y1+2, BW-4, BH-4, C_BG);

  tft.setTextWrap(false);
  tft.setFont(FONT_LAP_NUMBER);
  tft.setTextSize(1);

  // If total laps provided (>0) show "cur/total"
  if (totLaps > 0) {
    String s = String(curLap) + "/" + String(totLaps);

    int16_t bx, by; uint16_t bw, bh;
    tft.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
    int cx = LX + (BW - bw)/2 - bx;
    int cy = Y1 + (BH - bh)/2 - by;
    tft.setTextColor(C_TX);
    tft.setCursor(cx, cy); tft.print(s);

  } else {
    // No total provided: show "cur/∞" where ∞ is drawn graphically (two overlapping circles).
    String sLeft = String(curLap) + "/";

    int16_t bx, by; uint16_t bw, bh;
    tft.getTextBounds(sLeft, 0, 0, &bx, &by, &bw, &bh);

    // size parameters for the drawn infinity symbol
    const int r = 6;                 // circle radius
    const int overlap = r / 2;       // overlap between the two circles
    const int symbolW = (r*2) + (r*2 - overlap); // approximate symbol width
    const int spacing = 6;           // space between text and symbol

    int totalW = bw + spacing + symbolW;
    int tx = LX + (BW - totalW)/2 - bx;
    int ty = Y1 + (BH - bh)/2 - by;

    tft.setTextColor(C_TX);
    tft.setCursor(tx, ty); tft.print(sLeft);

    int symbolStart = tx + bw + spacing;
    int cx1 = symbolStart + r;
    int cx2 = symbolStart + r + (r - overlap);
    int ycenter = Y1 + (BH / 2);

    // draw two overlapping circles as the infinity glyph
    tft.drawCircle(cx1, ycenter, r, C_TX);
    tft.drawCircle(cx2, ycenter, r, C_TX);
    // optionally draw a small center connector to improve the likeness
    tft.fillCircle(cx1 + (r - overlap/2), ycenter, 2, C_TX);
  }

  // restore classic font state
  tft.setFont(); tft.setTextSize(1);

  // update change-tracking
  ccurLap = curLap;
  cTotLaps = totLaps;
}

// ================== LEDs ==================
void drawRevLEDs(){
  unsigned long now = millis();
  if (now - lastDataTime > 2000) return;

  static bool flashState = false; static unsigned long lastFlash = 0; const unsigned long flashPeriod = 250;
  static uint8_t lastMask = 0;
  uint8_t mask = (flagRed?8:0) | (flagYellow?4:0) | (flagBlue?2:0) | (flagGreen?1:0);
  if (mask != lastMask) { lastMask = mask; flashState=false; lastFlash=0; }

  if (mask){
    if (now - lastFlash > flashPeriod) { flashState = !flashState; lastFlash = now; }
    RgbColor c = flagRed ? dimColor(RgbColor(255,0,0)) :
                 flagYellow ? dimColor(RgbColor(255,140,0)) :
                 flagBlue ? dimColor(RgbColor(0,0,255)) :
                 dimColor(RgbColor(0,255,0));
    for (int i=0;i<LED_COUNT;i++) strip.SetPixelColor(i, flashState ? c : dimColor(RgbColor(0)));
    strip.Show();
    return;
  }

  if (rpm < 0 || maxRpm <= 0) { clearAllLeds(); return; }

  const float startPct = 0.75f;
  const float endPct   = 0.90f;

  float norm = (float)rpm / (float)maxRpm;
  float pct  = (norm - startPct) / (endPct - startPct);
  if (pct < 0) pct = 0; if (pct > 1) pct = 1;

  int target = round(pct * LED_COUNT);

  static int lastLit = 0;
  if (target > lastLit) lastLit++;
  else if (target < lastLit && (now & 1)) lastLit--;
  int lit = lastLit;

  for (int i=0; i<LED_COUNT; i++) {
    RgbColor c = dimColor(RgbColor(0));
    if (i < lit) {
      if (i == 0)      c = dimColor(RgbColor(0,255,0));
      else if (i <= 2) c = dimColor(RgbColor(255,140,0));
      else if (i <= 5) c = dimColor(RgbColor(255,0,0));
      else             c = dimColor(RgbColor(0,0,255));
    }
    strip.SetPixelColor(i, c);
  }
  strip.Show();
}

// ================== CSV ==================
void readCSV(){
  static String line;
  while (Serial.available()) {
    lastDataTime = millis();
    char ch = (char)Serial.read();
    if ((uint8_t)ch == 10) { // LF
      long v[15] = {0}; int i = 0; String tok = "";
      for (uint16_t k = 0; k < line.length() && i < 15; k++) {
        char c = line[k];
        if (c == ',') {
          tok.trim();
          v[i++] = tok.indexOf('.') >= 0 ? (long)tok.toFloat() : (long)tok.toInt();
          tok = "";
        } else if ((uint8_t)c != 13) { tok += c; }
      }
      tok.trim(); if (i < 15) v[i++] = tok.indexOf('.') >= 0 ? (long)tok.toFloat() : (long)tok.toInt();

      // We expect at least the original 13 fields. Additional trailing fields are optional:
      // [0] rpm, [1] speed, [2] gear, [3] pos, [4] fuel, [5] lapMs, [6] bestMs, [7] deltaMs,
      // [8] maxRpm, [9] flagYellow, [10] flagBlue, [11] flagRed, [12] flagGreen,
      // optional [13] CurrentLap, [14] TotalLaps
      if (i >= 13) {
        rpm     = (int)v[0];
        speed   = (int)v[1];
        gear    = (int)v[2];
        pos     = (int)v[3];
        fuel    = (int)v[4];
        lapMs   = v[5];
        bestMs  = v[6];
        deltaMs = v[7];
        maxRpm  = max(1000, (int)v[8]);
        flagYellow = (int)v[9];
        flagBlue   = (int)v[10];
        flagRed    = (int)v[11];
        flagGreen  = (int)v[12];

        if (i >= 14) curLap = (int)v[13];
        else curLap = 0;

        if (i >= 15) totLaps = (int)v[14];
        else totLaps = 0;
      }
      line = "";
    } else {
      line += ch;
      if (line.length() > 256) line = ""; // crude guard
    }
  }
}

// ================== BOOT ==================
void setup(){
  Serial.begin(115200);

  WiFi.mode(WIFI_OFF); esp_wifi_stop();
  #ifdef ARDUINO_ARCH_ESP32
    if (btStart()) btStop();
  #endif

  strip.Begin();
  clearAllLeds();

  tft.begin();
  tft.setRotation(1);
  tft.setTextWrap(false);
  tft.setTextSize(1);
  drawStatic();

  lastDataTime = millis();
}

// ================== MAIN LOOP ==================
void loop(){
  readCSV();

  if (millis() - lastDataTime > 2000) {
    if (!noDataScreenActive) { drawNoDataScreen(); noDataScreenActive = true; }
    setNoDataLeds();
    return;
  }

  if (noDataScreenActive) {
    drawStatic(); clearAllLeds(); noDataScreenActive = false;
    crpm=-1; cspeed=-1; cgear=-999; cpos=-1; cfuel=-1; clap=-1; cbest=-1; cdelta=LONG_MIN/2;
    ccurLap = -1; cTotLaps = -1;
  }

  drawLapCounter();   // top-left box now shows lap counter (cur/total) or cur/∞
  drawSpeedRpmStack();
  drawPosBig();
  drawFuel();
  drawGear();       // fixed font with margin
  drawLapBest();
  drawDelta();
  drawRevLEDs();
}
