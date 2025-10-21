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
const uint16_t C_BG  = ILI9341_BLACK;
const uint16_t C_TX  = ILI9341_WHITE;
const uint16_t C_BOX = ILI9341_YELLOW;
const uint16_t C_OK  = ILI9341_GREEN;
const uint16_t C_BAD = ILI9341_RED;

// ================== STATE ==================
// Telemetry stash. Fed by CSV.
int rpm=0, maxRpm=8000, speed=0, gear=0, pos=0;
long lapMs=0, bestMs=0, deltaMs=0;
int flagYellow=0, flagBlue=0, flagRed=0, flagGreen=0;

// Tyre temps.
int tyreFL = 0, tyreFR = 0, tyreRL = 0, tyreRR = 0;

// Lap counters.
int curLap = 0;
int totLaps = 0;

// Pit limiter (from SimHub).
int pitLimiterOn = 0;

// ================== CHANGE-TRACKING ==================
int crpm=-1, cspeed=-1, cgear=-999, cpos=-1;
long clap=-1, cbest=-1, cdelta=LONG_MIN/2;
int ccurLap = -1, cTotLaps = -1;
int cTyreFL = INT_MIN, cTyreFR = INT_MIN, cTyreRL = INT_MIN, cTyreRR = INT_MIN;

// ================== DATA FRESHNESS ==================
unsigned long lastDataTime = 0;
bool noDataScreenActive = false;
// Pit overlay state.
bool pitScreenActive = false;
// Track whether we've applied display inversion for pit mode
bool pitInvertActive = false;

// ================== LAYOUT ==================
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
static inline String msToStr(long ms) {
  bool neg = ms < 0; if (neg) ms = -ms;
  long t = ms/1000, mm = t/60, ss = t%60, sss = ms%1000;
  char b[20];
  snprintf(b, sizeof(b), "%s%ld:%02ld.%03ld", neg?"-":"", mm, ss, sss);
  return String(b);
}
void frame(int x,int y,int w,int h){ tft.drawRect(x,y,w,h,C_BOX); }

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

static void twoLine_onCanvasGap(GFXcanvas16& cv,int x,int y,int w,int h,
                                const GFXfont* topFont,const String& top,
                                const GFXfont* botFont,const String& bottom,
                                uint16_t col, uint16_t bg, int gap){
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
void drawNoDataScreen() {
  tft.fillScreen(C_BG);
  tft.setFont(); tft.setTextSize(3); tft.setTextColor(C_BAD);
  int16_t x1, y1; uint16_t w, h;
  String msg = "NO DATA FEED";
  tft.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
  int x = (320 - w) / 2; int y = (240 - h) / 2;
  tft.setCursor(x, y); tft.print(msg);
}

// ================== LED TASK / API ==================
// We'll drive the strip from a dedicated FreeRTOS task so LED updates
// are applied immediately and are not starved by heavy display SPI work.

// Task handle + desired-state variables (shared with main loop)
TaskHandle_t ledTaskHandle = NULL;
volatile int  led_desired_lit = 0;      // desired number of lit LEDs (0..LED_COUNT)
volatile uint8_t led_desired_mask = 0;  // priority flags mask: bit3=red,bit2=yellow,bit1=blue,bit0=green
volatile bool led_request_clear = false;
volatile bool led_no_data_mode = false;

// portMUX for atomic critical sections (required by IDF v5+)
portMUX_TYPE ledMux = portMUX_INITIALIZER_UNLOCKED;

// helper: notify the LED task that state changed
static inline void notifyLedTask() {
  if (ledTaskHandle) xTaskNotifyGive(ledTaskHandle);
}

// LED driving task
void ledTask(void *pv) {
  (void)pv;
  RgbColor lastColors[LED_COUNT];
  for (int i = 0; i < LED_COUNT; ++i) {
    lastColors[i] = dimColor(RgbColor(0));
    strip.SetPixelColor(i, lastColors[i]);
  }
  strip.Show();

  const unsigned long flashPeriod = 250;
  const unsigned long noDataBlinkPeriod = 1000;
  unsigned long lastFlash = 0;
  unsigned long lastNoDataBlink = 0;
  bool flashState = false;
  bool noDataOn = false;

  while (true) {
    // Wait for a notify, but wake periodically (30ms) to handle flashing and no-data blinking.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30)); // clears notification when returned

    unsigned long now = millis();

    // copy desired state atomically
    portENTER_CRITICAL(&ledMux);
    int lit = led_desired_lit;
    uint8_t mask = led_desired_mask;
    bool request_clear = led_request_clear;
    bool noDataMode = led_no_data_mode;
    portEXIT_CRITICAL(&ledMux);

    // If explicit clear requested, override everything
    if (request_clear) {
      bool needShow = false;
      RgbColor off = dimColor(RgbColor(0));
      for (int i = 0; i < LED_COUNT; ++i) {
        if (lastColors[i].R != off.R || lastColors[i].G != off.G || lastColors[i].B != off.B) {
          strip.SetPixelColor(i, off);
          lastColors[i] = off;
          needShow = true;
        }
      }
      if (needShow) strip.Show();
      // clear the request (atomically)
      portENTER_CRITICAL(&ledMux);
      led_request_clear = false;
      portEXIT_CRITICAL(&ledMux);
      continue;
    }

    // No-data special mode: blink only first and last LED red (like original)
    if (noDataMode) {
      if (now - lastNoDataBlink >= noDataBlinkPeriod) {
        lastNoDataBlink = now;
        noDataOn = !noDataOn;
      }
      RgbColor onColor = dimColor(RgbColor(255,0,0));
      RgbColor off = dimColor(RgbColor(0));
      bool needShow = false;
      for (int i = 0; i < LED_COUNT; ++i) {
        RgbColor out = off;
        if ((i == 0 || i == LED_COUNT - 1) && noDataOn) out = onColor;
        if (lastColors[i].R != out.R || lastColors[i].G != out.G || lastColors[i].B != out.B) {
          strip.SetPixelColor(i, out);
          lastColors[i] = out;
          needShow = true;
        }
      }
      if (needShow) strip.Show();
      continue;
    }

    // Priority: flashing flags
    if (mask) {
      if (now - lastFlash >= flashPeriod) {
        lastFlash = now;
        flashState = !flashState;
      }
      RgbColor c = dimColor(RgbColor(0));
      if (mask & 8)       c = dimColor(RgbColor(255,0,0));   // red
      else if (mask & 4)  c = dimColor(RgbColor(255,140,0)); // yellow/orange
      else if (mask & 2)  c = dimColor(RgbColor(0,0,255));   // blue
      else if (mask & 1)  c = dimColor(RgbColor(0,255,0));   // green

      bool needShow = false;
      RgbColor off = dimColor(RgbColor(0));
      for (int i = 0; i < LED_COUNT; ++i) {
        RgbColor out = (flashState ? c : off);
        if (lastColors[i].R != out.R || lastColors[i].G != out.G || lastColors[i].B != out.B) {
          strip.SetPixelColor(i, out);
          lastColors[i] = out;
          needShow = true;
        }
      }
      if (needShow) strip.Show();
      continue;
    }

    // Normal rev-bar behavior: lit LEDs from 0..LED_COUNT (lit is set via main code)
    if (lit < 0) lit = 0;
    if (lit > LED_COUNT) lit = LED_COUNT;

    bool needShow = false;
    for (int i = 0; i < LED_COUNT; ++i) {
      RgbColor c = dimColor(RgbColor(0));
      if (i < lit) {
        if (i == 0)           c = dimColor(RgbColor(0,255,0));        // G
        else if (i <= 2)      c = dimColor(RgbColor(255,140,0));      // O
        else if (i <= 5)      c = dimColor(RgbColor(255,0,0));        // R
        else                  c = dimColor(RgbColor(0,0,255));        // top = blue
        if (i >= LED_COUNT - 2) c = dimColor(RgbColor(0,0,255));      // force top two blue
      }
      if (lastColors[i].R != c.R || lastColors[i].G != c.G || lastColors[i].B != c.B) {
        strip.SetPixelColor(i, c);
        lastColors[i] = c;
        needShow = true;
      }
    }
    if (needShow) strip.Show();
  }
}

// --- Non-blocking setters (main loop uses these) ---
void setRevBarStateImmediate(int lit, uint8_t mask) {
  if (lit < 0) lit = 0;
  if (lit > LED_COUNT) lit = LED_COUNT;
  portENTER_CRITICAL(&ledMux);
  led_desired_lit = lit;
  led_desired_mask = mask;
  portEXIT_CRITICAL(&ledMux);
  notifyLedTask();
}

void requestClearLeds() {
  portENTER_CRITICAL(&ledMux);
  // also ensure no-data-mode is off when clearing
  led_no_data_mode = false;
  led_request_clear = true;
  portEXIT_CRITICAL(&ledMux);
  notifyLedTask();
}

void setNoDataLeds() {
  // request the special no-data blink mode
  portENTER_CRITICAL(&ledMux);
  led_no_data_mode = true;
  portEXIT_CRITICAL(&ledMux);
  notifyLedTask();
}

// ================== PIT LIMITER OVERLAY ==================
// New behaviour:
// - Keep the regular dash visible.
// - In the gear box area, blink a large 'P' on/off.
// - Invert the whole display colors while pitLimiterOn is active (hardware invert).
// This lets the driver glance at the dash during pit and still clearly see the pit state.

void drawPitLimiterOverlayBlink() {
  static unsigned long lastBlink = 0;
  static bool showText = false;
  unsigned long now = millis();
  const unsigned long period = 400; // blink rate

  if (now - lastBlink >= period) {
    lastBlink = now;
    showText = !showText;

    // inner gear box coordinates (match how cvGear is blitted)
    int gx = GX + 2;
    int gy = GY + 2;
    int gw = GW - 4;
    int gh = GH - 4;

    // Clear the gear box area first (use background so we don't get ghosting)
    tft.fillRect(gx, gy, gw, gh, C_BG);

    tft.setFont(FONT_GEAR); tft.setTextSize(1);
    tft.setTextColor(C_BOX);

    if (showText) {
      // Draw a big 'P' centered in the gear box.
      int16_t bx, by; uint16_t bw, bh;
      tft.getTextBounds("P", 0, 0, &bx, &by, &bw, &bh);
      int cx = gx + (gw - bw)/2 - bx;
      int cy = gy + (gh - bh)/2 - by;
      tft.setCursor(cx, cy);
      tft.print("P");
    } else {
      // Draw the current gear value centered in the gear box (this ensures
      // the overlay alternates between 'P' and the actual gear, not a stale 'N').
      String g = (gear < 0) ? "R" : (gear == 0) ? "N" : String(gear);
      int16_t bx, by; uint16_t bw, bh;
      tft.getTextBounds(g, 0, 0, &bx, &by, &bw, &bh);
      int cx = gx + (gw - bw)/2 - bx;
      int cy = gy + (gh - bh)/2 - by;
      tft.setCursor(cx, cy);
      tft.print(g);
    }

    tft.setFont(); tft.setTextSize(1);
  }
}

// ================== DYNAMIC DRAW (to canvases) ==================
void drawSpeedRpmStack(){
  static unsigned long lastDraw = 0;
  if (speed == cspeed && rpm == crpm) return;
  unsigned long now = millis();
  if (now - lastDraw < 25) return; // ~40 Hz

  cvSpeedRpm->fillScreen(C_BG);
  twoLine_onCanvasGap(*cvSpeedRpm, 0, 0, cvSpeedRpm->width(), cvSpeedRpm->height(),
                      FONT_SPEED, String(speed),
                      FONT_RPM,   String(rpm),
                      C_TX, C_BG, 12);   // was ~6, now roomier

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

  twoLine_onCanvas(*cvTyres, 0,      0,      halfW,      halfH,      FONT_TYRE_LABEL, "FL", FONT_TYRE_VALUE, String(tyreFL), C_TX, C_BG);
  twoLine_onCanvas(*cvTyres, halfW,  0,      boxW-halfW, halfH,      FONT_TYRE_LABEL, "FR", FONT_TYRE_VALUE, String(tyreFR), C_TX, C_BG);
  twoLine_onCanvas(*cvTyres, 0,      halfH,  halfW,      boxH-halfH, FONT_TYRE_LABEL, "RL", FONT_TYRE_VALUE, String(tyreRL), C_TX, C_BG);
  twoLine_onCanvas(*cvTyres, halfW,  halfH,  boxW-halfW, boxH-halfH, FONT_TYRE_LABEL, "RR", FONT_TYRE_VALUE, String(tyreRR), C_TX, C_BG);

  // Yellow cross overlay so each tyre sits in its own little box
  cvTyres->drawFastVLine(halfW, 0,     boxH, C_BOX);
  cvTyres->drawFastHLine(0,     halfH, boxW, C_BOX);

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

// Big fat “P#”.
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

// Delta
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
    // "cur/∞"
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

// ================== LEDs (main loop helper) ==================
// drawRevLEDs now only computes the desired lit count and mask and
// notifies the LED task which applies the update immediately.
void drawRevLEDs(){
  unsigned long now = millis();
  if (now - lastDataTime > 2000) return; // task handles no-data mode separately

  uint8_t mask = (flagRed ? 8 : 0) | (flagYellow ? 4 : 0) | (flagBlue ? 2 : 0) | (flagGreen ? 1 : 0);

  int lit = 0;
  if (rpm >= 0 && maxRpm > 0) {
    const float startPct = 0.80f;
    const float endPct   = 1.00f;

    float norm = (float)rpm / (float)maxRpm;
    float pct  = (norm - startPct) / (endPct - startPct);
    if (pct < 0) pct = 0; if (pct > 1) pct = 1;

    lit = roundf(pct * LED_COUNT);
    if (lit < 0) lit = 0; if (lit > LED_COUNT) lit = LED_COUNT;
  }

  setRevBarStateImmediate(lit, mask);
}

// ================== CSV ==================
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

        // Expect GameData.PitLimiterOn next if present.
        pitLimiterOn = (i >= 20) ? (int)v[19] : 0;
      }
      line = "";
    } else {
      line += ch;
      if (line.length() > 1024) line = "";
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

  // Start LED task so it owns strip.Show() and RMT timing.
  xTaskCreatePinnedToCore(ledTask, "ledTask", 4096, NULL, 2, &ledTaskHandle, 1);

  // Ensure LEDs start cleared
  requestClearLeds();

  tft.begin();
  // tft.setSPISpeed(40000000);
  tft.setRotation(1);
  tft.setTextWrap(false);
  tft.setTextSize(1);

  // Allocate canvases to exactly the inner sizes of the frames.
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
void loop(){
  readCSV();

  if (millis() - lastDataTime > 2000) {
    if (!noDataScreenActive) { drawNoDataScreen(); noDataScreenActive = true; }
    setNoDataLeds();
    return;
  }

  if (noDataScreenActive) {
    drawStatic();
    // clear leds and leave no-data mode off
    requestClearLeds();
    noDataScreenActive = false;
    crpm=-1; cspeed=-1; cgear=-999; cpos=-1; clap=-1; cbest=-1; cdelta=LONG_MIN/2;
    ccurLap = -1; cTotLaps = -1;
    cTyreFL = cTyreFR = cTyreRL = cTyreRR = INT_MIN;
  }

  // Pit limiter overlay: new behaviour — don't hide the whole dash.
  // Instead, invert the display colors while pitLimiterOn, and blink a 'P'
  // inside the gear box. LEDs continue to show revs.
  if (pitLimiterOn) {
    if (!pitScreenActive) {
      // entering pit; invert display for emphasis
      tft.invertDisplay(true);
      pitInvertActive = true;
      pitScreenActive = true;
    }
    drawPitLimiterOverlayBlink();
    drawRevLEDs(); // keep rev LEDs in pit (this quickly notifies LED task)
    return;
  } else if (pitScreenActive) {
    // leaving pit; restore normal appearance
    if (pitInvertActive) {
      tft.invertDisplay(false);
      pitInvertActive = false;
    }
    drawStatic();
    requestClearLeds();
    pitScreenActive = false;
    crpm=-1; cspeed=-1; cgear=-999; cpos=-1; clap=-1; cbest=-1; cdelta=LONG_MIN/2;
    ccurLap = -1; cTotLaps = -1;
    cTyreFL = cTyreFR = cTyreRL = cTyreRR = INT_MIN;
  }

  drawLapCounter();   // top-left
  drawPosBig();       // right-top
  drawLapBest();      // bottom-left
  drawDelta();        // bottom-right
  drawGear();         // center
  drawSpeedRpmStack();// left-bottom
  drawTyreTemps();    // right-bottom

  drawRevLEDs();      // LEDs: compute desired state and notify task
}
