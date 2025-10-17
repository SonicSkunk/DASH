/*
ESP32 SimHub Dash for ILI9341 (320x240) + 8x WS2812 LEDs

WHAT IT DOES
- Reads one CSV line per frame from Serial (115200). Field order:
  RPM,SPEED,GEAR,POSITION,FUEL,LAP_MS,BEST_MS,DELTA_MS,MAXRPM,FLAG_YELLOW,FLAG_BLUE,FLAG_RED,FLAG_GREEN
- Renders RPM/speed/fuel/gear/pos/times on ILI9341
- Drives WS2812 shift-light bar + race flags using ESP32 RMT
- Falls back to "NO DATA" screen if no serial data for 2 seconds
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
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <limits.h>

// ================== HARDWARE PINS ==================
#define TFT_CS   27
#define TFT_DC   16
#define TFT_RST   4

// WS2812 strip (8 LEDs by default). Use a GPIO with RMT support.
#define LED_PIN   13
#define LED_COUNT 8

// ILI9341 SPI display (hardware SPI used)
Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);

// WS2812 via ESP32 RMT @ 800 Kbps, GRB order (NeoPixelBus handles timing)
NeoPixelBus<NeoGrbFeature, NeoEsp32Rmt0800KbpsMethod> strip(LED_COUNT, LED_PIN);

// ================== LED BRIGHTNESS CAP ==================
const float LED_BRIGHTNESS = 0.07f;
static inline RgbColor dimColor(RgbColor c) {
  return RgbColor(
    (uint8_t)(c.R * LED_BRIGHTNESS),
    (uint8_t)(c.G * LED_BRIGHTNESS),
    (uint8_t)(c.B * LED_BRIGHTNESS)
  );
}

// ================== COLOR PRESETS ==================
const uint16_t C_BG  = ILI9341_BLACK;
const uint16_t C_TX  = ILI9341_WHITE;
const uint16_t C_BOX = ILI9341_YELLOW;
const uint16_t C_OK  = ILI9341_GREEN;
const uint16_t C_BAD = ILI9341_RED;

// ================== LIVE TELEMETRY STATE ==================
int rpm=0, maxRpm=8000, speed=0, gear=0, pos=0, fuel=0;
long lapMs=0, bestMs=0, deltaMs=0;
int flagYellow=0, flagBlue=0, flagRed=0, flagGreen=0;

// ================== CHANGE-TRACKING ==================
int crpm=-1, cspeed=-1, cgear=-999, cpos=-1, cfuel=-1;
long clap=-1, cbest=-1, cdelta=LONG_MIN/2;

// ================== DATA FRESHNESS ==================
unsigned long lastDataTime = 0;
bool noDataScreenActive = false;


// ================== LAYOUT (PIXELS) ==================
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

// ================== SMALL HELPERS ==================
static inline String msToStr(long ms) {
  bool neg = ms < 0; if (neg) ms = -ms;
  long t = ms/1000, mm = t/60, ss = t%60, sss = ms%1000;
  char b[20];
  snprintf(b, sizeof(b), "%s%ld:%02ld.%03ld", neg?"-":"", mm, ss, sss);
  return String(b);
}

void frame(int x,int y,int w,int h){ tft.drawRect(x,y,w,h,C_BOX); }

void caption(int x,int y,const char* s){
  tft.setFont();
  tft.setTextColor(C_TX);
  tft.setTextSize(2);
  tft.setCursor(x,y);
  tft.print(s);
}

// Draw text centered in a rectangle. Picks a font that fits, then prints.
static void drawCenteredText(int x,int y,int w,int h,const String& s,uint16_t col){
  const GFXfont* fonts[] = { &FreeSansBold24pt7b, &FreeSansBold18pt7b, &FreeSansBold12pt7b, nullptr };
  tft.fillRect(x+2,y+2,w-4,h-4,C_BG);
  int16_t bx,by; uint16_t bw,bh;
  for(int i=0; fonts[i]; i++){
    tft.setFont(fonts[i]);
    tft.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
    if(bw <= w-10 && bh <= h-10){
      tft.setTextColor(col);
      tft.setCursor(x + (w - bw)/2 - bx, y + (h - bh)/2 - by);
      tft.print(s);
      tft.setFont();
      return;
    }
  }
  // fallback tiny font if nothing fits
  tft.setFont(); tft.setTextSize(2);
  int cw = 6*2*s.length(), ch = 8*2;
  tft.setTextColor(col);
  tft.setCursor(x + (w - cw)/2, y + (h - ch)/2);
  tft.print(s);
  tft.setTextSize(1);
}

// One-time background boxes/labels
void drawStatic(){
  tft.fillScreen(C_BG);
  frame(LX,  Y1, BW, BH); caption(LX+6,  Y1+6, "RPM");
  frame(LX,  Y2, BW, BH); caption(LX+6,  Y2+6, "speed");
  frame(RXc, Y1, BW, BH);
  frame(RXc, Y2, BW, BH); caption(RXc+6, Y2+6, "fuel");
  frame(GX,  GY, GW, GH);
  frame(BLX, BOT_Y, BBW, BOT_H);
  frame(BRX, BOT_Y, BBW, BOT_H);
}

// Big red message when serial feed dies (no redraw spam)
void drawNoDataScreen() {
  tft.fillScreen(C_BG);
  tft.setFont(); tft.setTextSize(3); tft.setTextColor(C_BAD);
  int16_t x1, y1; uint16_t w, h;
  String msg = "NO DATA FEED";
  tft.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
  int x = (320 - w) / 2; int y = (240 - h) / 2;
  tft.setCursor(x, y); tft.print(msg);
}

// Slow blink first/last LEDs in red during NO DATA
void setNoDataLeds() {
  static unsigned long lastBlink=0;
  static bool on=false;
  static bool lastOn=false;
  unsigned long now=millis();

  // 1 second period
  if (now - lastBlink >= 1000) {
    lastBlink = now;
    on = !on;
  }

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

// Clears the whole LED bar fast
void clearAllLeds() {
  for (int i = 0; i < LED_COUNT; i++) strip.SetPixelColor(i, dimColor(RgbColor(0)));
  strip.Show();
}

// Each drawX only updates if the value changed to reduce flicker
void drawRPM(){
  if(rpm==crpm) return;
  tft.fillRect(LX+6, Y1+28, BW-12, BH-30, C_BG);
  tft.setFont(); tft.setTextColor(C_TX); tft.setTextSize(2);
  tft.setCursor(LX+8, Y1+32); tft.print(rpm);
  crpm=rpm;
}

void drawSpeed(){
  if(speed==cspeed) return;
  tft.fillRect(LX+6, Y2+28, BW-12, BH-30, C_BG);
  tft.setFont(); tft.setTextColor(C_TX); tft.setTextSize(2);
  tft.setCursor(LX+8, Y2+32); tft.print(speed);
  cspeed=speed;
}

void drawFuel(){
  if(fuel==cfuel) return;
  tft.fillRect(RXc+6, Y2+28, BW-12, BH-30, C_BG);
  tft.setFont(); tft.setTextColor(C_TX); tft.setTextSize(2);
  tft.setCursor(RXc+8, Y2+32); tft.print(fuel); tft.print('%');
  cfuel=fuel;
}

void drawGear(){
  if(gear==cgear) return;
  String g = (gear<0) ? "R" : (gear==0) ? "N" : String(gear);
  drawCenteredText(GX,GY,GW,GH,g,C_BOX);
  cgear=gear;
}

void drawPosBig(){
  if(pos==cpos) return;
  String s = String("P") + String(pos);
  drawCenteredText(RXc+2, Y1+2, BW-4, BH-4, s, C_TX);
  cpos=pos;
}

// Bottom-left box: last and best lap, right-aligned
void drawLapBest() {
bool hasLast = lapMs > 0;
bool hasBest = bestMs > 0;
if (lapMs == clap && bestMs == cbest) return;


int x0 = BLX + 4, y0 = BOT_Y + 3, w = BBW - 8, h = BOT_H - 6;
tft.fillRect(x0, y0, w, h, C_BG);


// Labels smaller, positioned left
tft.setFont();
tft.setTextColor(C_TX);
tft.setTextSize(1);


const int labelOffsetX = 6;
const int labelOffsetY = 5;
tft.setCursor(x0 + labelOffsetX, y0 + labelOffsetY + 8);
tft.print("last:");
tft.setCursor(x0 + labelOffsetX, y0 + h / 2 + labelOffsetY);
tft.print("best:");


// Larger font for times
String sLast = hasLast ? msToStr(lapMs) : String("--:--.---");
String sBest = hasBest ? msToStr(bestMs) : String("--:--.---");
tft.setFont(&FreeSansBold12pt7b);


int16_t bx, by; uint16_t bw, bh;
int rightEdge = x0 + w - 6;


// Adjusted Y positions to bring bottom time closer to box bottom
tft.getTextBounds(sLast, 0, 0, &bx, &by, &bw, &bh);
int baseY1 = y0 + (h / 4) + (bh / 2) - 2;
tft.setCursor(rightEdge - bw, baseY1);
tft.print(sLast);


tft.getTextBounds(sBest, 0, 0, &bx, &by, &bw, &bh);
int baseY2 = y0 + (3 * h / 4) + (bh / 2) - 1; // slightly closer to bottom edge
tft.setCursor(rightEdge - bw, baseY2);
tft.print(sBest);


tft.setFont();
tft.setTextSize(1);
clap = lapMs;
cbest = bestMs;
}

// Bottom-right box: delta to reference. Green = gaining, Red = losing.
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
tft.setFont(&FreeSansBold18pt7b);
tft.setTextColor(col);


int16_t bx, by; uint16_t bw, bh;
tft.getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
int cx = x0 + (w - bw) / 2 - bx;
int cy = y0 + (h - bh) / 2 - by; // fully centered vertically
tft.setCursor(cx, cy);
tft.print(buf);


tft.setFont();
tft.setTextSize(1);
cdelta = deltaMs;
}

// Shift light + flags
// - Flags override: if any flag set, whole bar blinks that color.
// - Otherwise: progressive bar from 60% to 90% of maxRpm.
void drawRevLEDs(){
  unsigned long now = millis();// If we haven't had data recently, don't animate revs here.
  if (now - lastDataTime > 2000) return;

  // Flag override with blink
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

  // Progressive rev bar
  if (rpm < 0 || maxRpm <= 0) { clearAllLeds(); return; }

  const float startPct = 0.75f; // 
  const float endPct   = 0.90f;

  float norm = (float)rpm / (float)maxRpm;
  float pct  = (norm - startPct) / (endPct - startPct);
  if (pct < 0) pct = 0; if (pct > 1) pct = 1;

  int target = round(pct * LED_COUNT);

  // simple easing
  static int lastLit = 0;
  if (target > lastLit) lastLit++;
  else if (target < lastLit && (now & 1)) lastLit--;
  int lit = lastLit;

  for (int i=0; i<LED_COUNT; i++) {
    RgbColor c = dimColor(RgbColor(0));
    if (i < lit) {
      // Color zones: 0=green, 1-2=amber, 3-5=red, 6+=blue
      if (i == 0)      c = dimColor(RgbColor(0,255,0));
      else if (i <= 2) c = dimColor(RgbColor(255,140,0));
      else if (i <= 5) c = dimColor(RgbColor(255,0,0));
      else             c = dimColor(RgbColor(0,0,255));
    }
    strip.SetPixelColor(i, c);
  }
  strip.Show();
}

// ================== CSV INPUT ==================
void readCSV(){
  static String line;
  while (Serial.available()) {
lastDataTime = millis();
    char ch = (char)Serial.read();
    if ((uint8_t)ch == 10) { // LF
      long v[13] = {0}; int i = 0; String tok = "";
      for (uint16_t k = 0; k < line.length() && i < 13; k++) {
        char c = line[k];
        if (c == ',') {
          tok.trim();
          v[i++] = tok.indexOf('.') >= 0 ? (long)tok.toFloat() : (long)tok.toInt();
          tok = "";
        } else if ((uint8_t)c != 13) { // skip CR
          tok += c;
        }
      }
      tok.trim(); if (i < 13) v[i++] = tok.indexOf('.') >= 0 ? (long)tok.toFloat() : (long)tok.toInt();

      if (i == 13) {
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

  // Kill Wi-Fi/BT
  WiFi.mode(WIFI_OFF); esp_wifi_stop();
  #ifdef ARDUINO_ARCH_ESP32
    if (btStart()) btStop();
  #endif

  // Init LEDs early and once
  strip.Begin();
  clearAllLeds();

  // Init TFT
  tft.begin();
  tft.setRotation(1);
  drawStatic();

  // Avoid instant NO DATA before init
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
    // Data resumed: redraw base UI and force refresh
    drawStatic(); clearAllLeds(); noDataScreenActive = false;
    crpm=-1; cspeed=-1; cgear=-999; cpos=-1; cfuel=-1; clap=-1; cbest=-1; cdelta=LONG_MIN/2;
  }

  drawRPM();
  drawSpeed();
  drawPosBig();
  drawFuel();
  drawGear();
  drawLapBest();
  drawDelta();
  drawRevLEDs();
}
