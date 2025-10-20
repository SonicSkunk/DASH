/*
ESP32 SimHub Dash for ILI9341 (320x240) + 8x WS2812 LEDs
Renders RPM, speed, lap counter, gear, position, tyre temperatures, lap/best, and delta on an ILI9341. 
Drives an 8-LED shift bar via ESP32 RMT. 
Falls back to NO DATA FEED when input stops.
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

// Included fonts.
#include <Fonts/FreeSansBold66pt7b.h>
#include <Fonts/FreeSansBold48pt7b.h>
#include <Fonts/FreeSansBold36pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans8pt7b.h>
#include <Fonts/FreeSans10pt7b.h>

#include <limits.h>

// ================== HARDWARE PINS ==================
// ILI9341 SPI pins. Match your wiring.
#define TFT_CS   27
#define TFT_DC   16
#define TFT_RST   4

// WS2812 led strip.
#define LED_PIN   13
#define LED_COUNT 8

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
NeoPixelBus<NeoGrbFeature, NeoEsp32Rmt0800KbpsMethod> strip(LED_COUNT, LED_PIN);

// ================== LED BRIGHTNESS ==================
// Global LED brightness scaler. 0.07 keeps it visible without flashbanging yourself mid-corner.
const float LED_BRIGHTNESS = 0.07f;
static inline RgbColor dimColor(RgbColor c) {
  return RgbColor(
    (uint8_t)(c.R * LED_BRIGHTNESS),
    (uint8_t)(c.G * LED_BRIGHTNESS),
    (uint8_t)(c.B * LED_BRIGHTNESS)
  );
}

// ================== COLORS ==================
// House palette.
const uint16_t C_BG  = ILI9341_BLACK;
const uint16_t C_TX  = ILI9341_WHITE;
const uint16_t C_BOX = ILI9341_YELLOW;
const uint16_t C_OK  = ILI9341_GREEN;
const uint16_t C_BAD = ILI9341_RED;

// ================== STATE ==================
// Telemetry stash. Fed by CSV. If you see nonsense, your input is nonsense.
int rpm=0, maxRpm=8000, speed=0, gear=0, pos=0;
long lapMs=0, bestMs=0, deltaMs=0;
int flagYellow=0, flagBlue=0, flagRed=0, flagGreen=0;

// Tyre temps.
int tyreFL = 0, tyreFR = 0, tyreRL = 0, tyreRR = 0;

// Lap counters.
int curLap = 0;
int totLaps = 0;

// ================== CHANGE-TRACKING ==================
// Cached values so we only redraw when something actually changes.
int crpm=-1, cspeed=-1, cgear=-999, cpos=-1;
long clap=-1, cbest=-1, cdelta=LONG_MIN/2;
int ccurLap = -1, cTotLaps = -1;
int cTyreFL = INT_MIN, cTyreFR = INT_MIN, cTyreRL = INT_MIN, cTyreRR = INT_MIN;

// ================== DATA FRESHNESS ==================
// If the feed goes quiet, we show a 'NO DATA FEED' screen and blink LEDs.
unsigned long lastDataTime = 0;
bool noDataScreenActive = false;

// ================== LAYOUT ==================
// Pixel math. Touch this if you enjoy UI Tetris.
const int M  = 6;     // margin
const int G  = 8;     // gap
const int BW = 92;    // side box width
const int BH = 56;    // top box height

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

// Speed/RPM area uses whatever is left between Y2 and bottom row.
static inline int BH_speedFuel() { return (GY + GH) - Y2; }

// ================== USED FONTS ==================
static const GFXfont* FONT_GEAR       = &FreeSansBold66pt7b;
static const GFXfont* FONT_SPEED      = &FreeSansBold24pt7b;
static const GFXfont* FONT_RPM        = &FreeSans10pt7b;
static const GFXfont* FONT_POS        = &FreeSansBold24pt7b;
static const GFXfont* FONT_LAPBEST    = &FreeSansBold12pt7b;
static const GFXfont* FONT_DELTA      = &FreeSansBold18pt7b;
static const GFXfont* FONT_TYRE_VALUE = &FreeSansBold12pt7b;
static const GFXfont* FONT_TYRE_LABEL = &FreeSans8pt7b;
static const GFXfont* FONT_LAP_NUMBER = &FreeSansBold18pt7b;

// ================== HELPERS ==================
// Converts milliseconds to mm:ss.mmm
static inline String msToStr(long ms) {
  bool neg = ms < 0; if (neg) ms = -ms;
  long t = ms/1000, mm = t/60, ss = t%60, sss = ms%1000;
  char b[20];
  snprintf(b, sizeof(b), "%s%ld:%02ld.%03ld", neg?"-":"", mm, ss, sss);
  return String(b);
}
// Draws a neat box outline.
void frame(int x,int y,int w,int h){ tft.drawRect(x,y,w,h,C_BOX); }

// Canvas-only helpers. They measure text and center it.
static void centeredText_onCanvas(GFXcanvas16& cv,int x,int y,int w,int h,const GFXfont* font,const String& s,uint16_t col, uint16_t bg){
  cv.fillRect(x+2,y+2,w-4,h-4,bg);
  cv.setTextWrap(false);
  cv.setFont(font);
  cv.setTextSize(1);
  int16_t bx,by; uint16_t bw,bh;
  cv.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  int cx = x + (w - bw)/2 - bx;
  int cy = y + (h - bh)/2 - by;
  cv.setTextColor(col);
  cv.setCursor(cx, cy);
  cv.print(s);
  cv.setFont();
  cv.setTextSize(1);
}

static void twoLine_onCanvas(GFXcanvas16& cv,int x,int y,int w,int h,const GFXfont* topFont,const String& top,const GFXfont* botFont,const String& bottom,uint16_t col, uint16_t bg){
  const int gap = 6;
  cv.fillRect(x+2,y+2,w-4,h-4,bg);
  cv.setTextWrap(false);

  cv.setFont(topFont); cv.setTextSize(1);
  int16_t tbx,tby; uint16_t tbw,tbh;
  cv.getTextBounds(top, 0, 0, &tbx, &tby, &tbw, &tbh);

  cv.setFont(botFont); cv.setTextSize(1);
  int16_t bbx,bby; uint16_t bbw,bbh;
  cv.getTextBounds(bottom, 0, 0, &bbx, &bby, &bbw, &bbh);

  int totalH = (int)tbh + gap + (int)bbh;
  int blockTop = y + (h - totalH)/2;

  int topBaseline = blockTop - tby;
  int botBaseline = blockTop + tbh + gap - bby;

  int cxTop = x + (w - tbw)/2 - tbx;
  int cxBot = x + (w - bbw)/2 - bbx;

  cv.setTextColor(col);
  cv.setFont(topFont); cv.setCursor(cxTop, topBaseline); cv.print(top);
  cv.setFont(botFont); cv.setCursor(cxBot, botBaseline); cv.print(bottom);

  cv.setFont(); cv.setTextSize(1);
}

// ================== OFFSCREEN CANVASES ==================
// Each widget gets its own canvas. We draw there, then blit once.
GFXcanvas16 *cvLap      = nullptr; // Lap counter
GFXcanvas16 *cvSpeedRpm = nullptr; // Speed + RPM
GFXcanvas16 *cvPos      = nullptr; // Position
GFXcanvas16 *cvTyres    = nullptr; // Tyre temps
GFXcanvas16 *cvGear     = nullptr; // Gear
GFXcanvas16 *cvLapBest  = nullptr; // Last/Best times
GFXcanvas16 *cvDelta    = nullptr; // Delta

inline void blit(int x,int y,GFXcanvas16& c){
  tft.drawRGBBitmap(x, y, c.getBuffer(), c.width(), c.height());
}

// ================== STATIC BACKGROUND ==================
// Draw once. If you’re redrawing this every frame, expect a flicker fest.
void drawStatic(){
  tft.fillScreen(C_BG);

  // Left column
  frame(LX,  Y1, BW, BH);              // lap counter
  frame(LX,  Y2, BW, BH_speedFuel());  // speed+rpm

  // Right column
  frame(RXc, Y1, BW, BH);              // position
  frame(RXc, Y2, BW, BH_speedFuel());  // tyres

  // Center gear box
  frame(GX,  GY, GW, GH);

  // Bottom row
  frame(BLX, BOT_Y, BBW, BOT_H);       // lap/best
  frame(BRX, BOT_Y, BBW, BOT_H);       // delta
}

// ================== NO DATA ==================
// 'NO DATA FEED' screen when no data input.
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

// ================== DYNAMIC DRAW (to canvases) ==================
// Speed + RPM stacked.
void drawSpeedRpmStack(){
  static unsigned long lastDraw = 0;
  if (speed == cspeed && rpm == crpm) return;
  unsigned long now = millis();
  if (now - lastDraw < 25) return; // ~40 Hz

  cvSpeedRpm->fillScreen(C_BG);
  twoLine_onCanvas(*cvSpeedRpm, 0, 0, cvSpeedRpm->width(), cvSpeedRpm->height(),
                   FONT_SPEED, String(speed), FONT_RPM, String(rpm), C_TX, C_BG);

  blit(LX+2, Y2+2, *cvSpeedRpm);
  cspeed = speed; crpm = rpm; lastDraw = now;
}

// Tyres. Four boxes, two lines each.
void drawTyreTemps() {
  static unsigned long lastDraw = 0;
  if (tyreFL == cTyreFL && tyreFR == cTyreFR && tyreRL == cTyreRL && tyreRR == cTyreRR) return;
  unsigned long now = millis();
  if (now - lastDraw < 33) return; // ~30 Hz

  cvTyres->fillScreen(C_BG);

  int boxW = cvTyres->width();
  int boxH = cvTyres->height();
  int halfW = boxW / 2;
  int halfH = boxH / 2;

  twoLine_onCanvas(*cvTyres, 0, 0, halfW, halfH,  FONT_TYRE_LABEL, "FL", FONT_TYRE_VALUE, String(tyreFL), C_TX, C_BG);
  twoLine_onCanvas(*cvTyres, halfW, 0, boxW-halfW, halfH,  FONT_TYRE_LABEL, "FR", FONT_TYRE_VALUE, String(tyreFR), C_TX, C_BG);
  twoLine_onCanvas(*cvTyres, 0, halfH, halfW, boxH-halfH,  FONT_TYRE_LABEL, "RL", FONT_TYRE_VALUE, String(tyreRL), C_TX, C_BG);
  twoLine_onCanvas(*cvTyres, halfW, halfH, boxW-halfW, boxH-halfH,  FONT_TYRE_LABEL, "RR", FONT_TYRE_VALUE, String(tyreRR), C_TX, C_BG);

  blit(RXc+2, Y2+2, *cvTyres);

  cTyreFL = tyreFL; cTyreFR = tyreFR; cTyreRL = tyreRL; cTyreRR = tyreRR;
  lastDraw = now;
}

// Gear in the big center box.
void drawGear(){
  static unsigned long lastDraw = 0;
  if (gear == cgear) return;
  unsigned long now = millis();
  if (now - lastDraw < 25) return;

  String g = (gear<0) ? "R" : (gear==0) ? "N" : String(gear);

  cvGear->fillScreen(C_BG);
  cvGear->setTextWrap(false);
  cvGear->setFont(FONT_GEAR);
  cvGear->setTextSize(1);

  int16_t bx, by; uint16_t bw, bh;
  cvGear->getTextBounds(g, 0, 0, &bx, &by, &bw, &bh);

  int cx = (cvGear->width()  - bw)/2 - bx;
  int cy = (cvGear->height() - bh)/2 - by;

  cvGear->setTextColor(C_BOX);
  cvGear->setCursor(cx, cy);
  cvGear->print(g);
  cvGear->setFont(); cvGear->setTextSize(1);

  blit(GX+2, GY+2, *cvGear);

  cgear = gear; lastDraw = now;
}

// Big fat “P#”. If you’re not P1, drive faster.
void drawPosBig(){
  if(pos==cpos) return;

  cvPos->fillScreen(C_BG);
  String s = String("P") + String(pos);
  centeredText_onCanvas(*cvPos, 0, 0, cvPos->width(), cvPos->height(), FONT_POS, s, C_TX, C_BG);

  blit(RXc+2, Y1+2, *cvPos);

  cpos=pos;
}

// Last and Best lap times.
void drawLapBest() {
  bool hasLast = lapMs > 0;
  bool hasBest = bestMs > 0;
  if (lapMs == clap && bestMs == cbest) return;

  cvLapBest->fillScreen(C_BG);

  String sLast = hasLast ? msToStr(lapMs) : String("--:--.---");
  String sBest = hasBest ? msToStr(bestMs) : String("--:--.---");

  cvLapBest->setFont(FONT_LAPBEST); cvLapBest->setTextSize(1);
  int16_t tbxL, tbyL; uint16_t tbwL, tbhL;
  cvLapBest->getTextBounds(sLast, 0, 0, &tbxL, &tbyL, &tbwL, &tbhL);

  int16_t tbxB, tbyB; uint16_t tbwB, tbhB;
  cvLapBest->getTextBounds(sBest, 0, 0, &tbxB, &tbyB, &tbwB, &tbhB);

  int rightEdge = cvLapBest->width() - 6;
  int baseY1 = (cvLapBest->height() / 4) + (tbhL / 2) - 2;
  int baseY2 = (3 * cvLapBest->height() / 4) + (tbhB / 2) - 1;

  cvLapBest->setFont(FONT_TYRE_LABEL); cvLapBest->setTextSize(1);
  int16_t lbx, lby; uint16_t lbw, lbh;
  cvLapBest->getTextBounds("last:", 0, 0, &lbx, &lby, &lbw, &lbh);
  int16_t lbx2, lby2; uint16_t lbw2, lbh2;
  cvLapBest->getTextBounds("best:", 0, 0, &lbx2, &lby2, &lbw2, &lbh2);

  const int labelOffsetX = 6;
  int labelY1 = baseY1 + ((int)tbhL - (int)lbh) / 2;
  int labelY2 = baseY2 + ((int)tbhB - (int)lbh2) / 2;

  cvLapBest->setTextColor(C_TX);
  cvLapBest->setCursor(labelOffsetX - lbx, labelY1); cvLapBest->print("last:");
  cvLapBest->setCursor(labelOffsetX - lbx2, labelY2); cvLapBest->print("best:");

  cvLapBest->setFont(FONT_LAPBEST); cvLapBest->setTextSize(1);
  cvLapBest->getTextBounds(sLast, 0, 0, &tbxL, &tbyL, &tbwL, &tbhL);
  cvLapBest->setCursor(rightEdge - tbwL, baseY1); cvLapBest->print(sLast);

  cvLapBest->getTextBounds(sBest, 0, 0, &tbxB, &tbyB, &tbwB, &tbhB);
  cvLapBest->setCursor(rightEdge - tbwB, baseY2); cvLapBest->print(sBest);

  cvLapBest->setFont(); cvLapBest->setTextSize(1);

  blit(BLX+4, BOT_Y+3, *cvLapBest);

  clap = lapMs; cbest = bestMs;
}

// Delta with sign, centered and color-coded. Green good, red bad, shocker.
static const int DELTA_BASELINE_FIX = 0;
void drawDelta() {
  if (deltaMs == cdelta) return;

  cvDelta->fillScreen(C_BG);

  long v = deltaMs; unsigned long a = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
  int whole = a / 1000; int frac = a % 1000; if (whole > 99) { whole = 99; frac = 999; }
  char sign = (v < 0 ? '-' : (v > 0 ? '+' : '±'));

  char buf[12];
  if (whole < 10) snprintf(buf, sizeof(buf), "%c %d.%03d", sign, whole, frac);
  else snprintf(buf, sizeof(buf), "%c%02d.%03d", sign, whole, frac);

  uint16_t col = (v < 0) ? C_OK : (v > 0 ? C_BAD : C_TX);

  cvDelta->setTextSize(1);
  cvDelta->setFont(FONT_DELTA);
  cvDelta->setTextColor(col);

  int16_t bx, by; uint16_t bw, bh;
  cvDelta->getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
  int cx = (cvDelta->width() - bw) / 2 - bx;
  int cy = (cvDelta->height() - bh) / 2 - by + DELTA_BASELINE_FIX;
  cvDelta->setCursor(cx, cy); cvDelta->print(buf);

  cvDelta->setFont(); cvDelta->setTextSize(1);

  blit(BRX+4, BOT_Y+3, *cvDelta);

  cdelta = deltaMs;
}

// ================== LAP COUNTER ==================
// Shows "cur/total". If total is 0, draws a infinity symbol.
void drawLapCounter() {
  if (curLap == ccurLap && totLaps == cTotLaps) return;

  cvLap->fillScreen(C_BG);

  if (totLaps > 0) {
    String s = String(curLap) + "/" + String(totLaps);

    cvLap->setFont(FONT_LAP_NUMBER);
    cvLap->setTextSize(1);
    int16_t bx, by; uint16_t bw, bh;
    cvLap->getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
    int cx = (cvLap->width() - bw)/2 - bx;
    int cy = (cvLap->height() - bh)/2 - by;
    cvLap->setTextColor(C_TX);
    cvLap->setCursor(cx, cy); cvLap->print(s);
    cvLap->setFont(); cvLap->setTextSize(1);

  } else {
    // "cur/∞" with a DIY infinity.
    String sLeft = String(curLap) + "/";
    cvLap->setFont(FONT_LAP_NUMBER);
    cvLap->setTextSize(1);
    int16_t bx, by; uint16_t bw, bh;
    cvLap->getTextBounds(sLeft, 0, 0, &bx, &by, &bw, &bh);

    const int r = 6;
    const int overlap = r / 2;
    const int symbolW = (r*2) + (r*2 - overlap);
    const int spacing = 6;

    int totalW = bw + spacing + symbolW;
    int tx = (cvLap->width() - totalW)/2 - bx;
    int ty = (cvLap->height() - bh)/2 - by;

    cvLap->setTextColor(C_TX);
    cvLap->setCursor(tx, ty); cvLap->print(sLeft);

    int symbolStart = tx + bw + spacing;
    int cx1 = symbolStart + r;
    int cx2 = symbolStart + r + (r - overlap);
    int ycenter = cvLap->height() / 2;

    cvLap->drawCircle(cx1, ycenter, r, C_TX);
    cvLap->drawCircle(cx2, ycenter, r, C_TX);
    cvLap->fillCircle(cx1 + (r - overlap/2), ycenter, 2, C_TX);

    cvLap->setFont(); cvLap->setTextSize(1);
  }

  blit(LX+2, Y1+2, *cvLap);

  ccurLap = curLap;
  cTotLaps = totLaps;
}

// ================== LEDs ==================
// Flags override everything and blink. Otherwise, ease into a staged rev bar.
// Adjust startPct/endPct if you enjoy shifting at 3k like it’s 1998.
void drawRevLEDs(){
  unsigned long now = millis();
  if (now - lastDataTime > 2000) return;

  static bool flashState = false; 
  static unsigned long lastFlash = 0; 
  const unsigned long flashPeriod = 250;
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

  const float startPct = 0.75f; // first LED wakes up here
  const float endPct   = 0.90f; // full bar by here, shift or pray

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
// Expects up to 19 fields (we read up to 25 just in case):
// 0 rpm, 1 speed, 2 gear, 3 pos, 5 lapMs, 6 bestMs, 7 deltaMs, 8 maxRpm,
// 9-12 flags(Y,B,R,G), 13 curLap, 14 totLaps, 15-18 tyre FL/FR/RL/RR.
// If your feed is different, fix your sender. Or map here.
void readCSV(){
  static String line;
  while (Serial.available()) {
    lastDataTime = millis();
    char ch = (char)Serial.read();
    if ((uint8_t)ch == 10) { // LF terminator
      long v[25] = {0}; int i = 0; String tok = "";
      for (uint16_t k = 0; k < line.length() && i < 25; k++) {
        char c = line[k];
        if (c == ',') {
          tok.trim();
          v[i++] = tok.indexOf('.') >= 0 ? (long)tok.toFloat() : (long)tok.toInt();
          tok = "";
        } else if ((uint8_t)c != 13) { tok += c; }
      }
      tok.trim(); if (i < 25) v[i++] = tok.indexOf('.') >= 0 ? (long)tok.toFloat() : (long)tok.toInt();

      if (i >= 13) {
        rpm     = (int)v[0];
        speed   = (int)v[1];
        gear    = (int)v[2];
        pos     = (int)v[3];
        lapMs   = v[5];
        bestMs  = v[6];
        deltaMs = v[7];
        maxRpm  = max(1000, (int)v[8]);
        flagYellow = (int)v[9];
        flagBlue   = (int)v[10];
        flagRed    = (int)v[11];
        flagGreen  = (int)v[12];

        curLap  = (i >= 14) ? (int)v[13] : 0;
        totLaps = (i >= 15) ? (int)v[14] : 0;

        if (i >= 16) tyreFL = (int)v[15];
        if (i >= 17) tyreFR = (int)v[16];
        if (i >= 18) tyreRL = (int)v[17];
        if (i >= 19) tyreRR = (int)v[18];
      }
      line = "";
    } else {
      line += ch;
      if (line.length() > 1024) line = ""; // if your app spams garbage, we bail
    }
  }
}

// ================== BOOT ==================
// Kills WiFi/BT, starts LEDs/TFT, allocates canvases, draws static chrome. Done.
void setup(){
  Serial.begin(115200);

  WiFi.mode(WIFI_OFF); esp_wifi_stop();
  #ifdef ARDUINO_ARCH_ESP32
    if (btStart()) btStop();
  #endif

  strip.Begin();
  clearAllLeds();

  tft.begin();
  // tft.setSPISpeed(40000000); // Use if your ILI9341 lib supports it and your wiring isn’t a meme.
  tft.setRotation(1);
  tft.setTextWrap(false);
  tft.setTextSize(1);

  // Allocate canvases to exactly the inner sizes of the frames. No wasted pixels.
  cvLap      = new GFXcanvas16(BW-4,               BH-4);
  cvSpeedRpm = new GFXcanvas16(BW-4,               BH_speedFuel()-4);
  cvPos      = new GFXcanvas16(BW-4,               BH-4);
  cvTyres    = new GFXcanvas16(BW-4,               BH_speedFuel()-4);
  cvGear     = new GFXcanvas16(GW-4,               GH-4);
  cvLapBest  = new GFXcanvas16(BBW-8,              BOT_H-6);
  cvDelta    = new GFXcanvas16(BBW-8,              BOT_H-6);

  drawStatic();
  lastDataTime = millis();
}

// ================== MAIN LOOP ==================
// Read feed, handle no-data mode, then redraw only what changed. LEDs last.
void loop(){
  readCSV();

  if (millis() - lastDataTime > 2000) {
    if (!noDataScreenActive) { drawNoDataScreen(); noDataScreenActive = true; }
    setNoDataLeds();
    return;
  }

  if (noDataScreenActive) {
    drawStatic(); clearAllLeds(); noDataScreenActive = false;
    crpm=-1; cspeed=-1; cgear=-999; cpos=-1; clap=-1; cbest=-1; cdelta=LONG_MIN/2;
    ccurLap = -1; cTotLaps = -1;
    cTyreFL = cTyreFR = cTyreRL = cTyreRR = INT_MIN;
  }

  // Draw widgets (only blit on change; canvases avoid flicker)
  drawLapCounter();   // top-left
  drawPosBig();       // right-top
  drawLapBest();      // bottom-left
  drawDelta();        // bottom-right
  drawGear();         // center
  drawSpeedRpmStack();// left-bottom
  drawTyreTemps();    // right-bottom

  // LEDs last.
  drawRevLEDs();
}
