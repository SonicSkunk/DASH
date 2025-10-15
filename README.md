# ESP32 SimHub Dash (ILI9341 320×240 + WS2812) — Early Development

**Status:** early prototype. Functional, changing fast.

ESP32 dashboard for SimHub telemetry. Renders RPM, speed, gear, position, fuel, lap/best, and delta on an ILI9341. 
Drives an 8-LED shift bar via ESP32 RMT. Falls back to **NO DATA FEED** when input stops.

---

## Features
- RMT-driven WS2812.
- Incremental TFT redraws (only when values change).
- Delta readout with color coding. Green for gaining and red for losing time to reference.
- Flag override on LED bar (red, amber, blue, green).
- No-data fail mode: big warning + LED end-blink.
- Easy pin/brightness/layout tuning in code.

---

## Hardware
| MCU | ESP32 | Any with RMT support |
| Display | ILI9341, 320×240, SPI | Hardware SPI |
| LEDs | WS2812 | Default 8 LED bar |

### Default Pins
| TFT_CS | 27 |
| TFT_DC | 16 |
| TFT_RST | 4 |
| WS2812 DIN | 13 |

SPI uses the ESP32 hardware SPI (VSPI). Wire SCK/MOSI/MISO per your board.

## Libraries
Install via Arduino Library Manager:
- `Adafruit_GFX`
- `Adafruit_ILI9341`
- `NeoPixelBus`

ESP32 Arduino core required.

---

## Build & Flash
1. Select an ESP32 board in Arduino IDE.
2. Open the sketch, keep serial at **115200**.
3. Flash.

---

## Case & Mount (planned)
A custom **3D-printed case and mounting system** is in development.  
Once finalized, **STL and project files will be published** in this repo.

---

## Power Notes
- Power LEDs from a stable **5 V** rail (not the ESP32 3.3 V pin).
- Common GND across ESP32, TFT, and LEDs.
- Add ≥470 µF across LED 5 V/GND near the strip.

---

## Known Issues (WIP)
- Occasional LED lag if the serial feed is inconsistent.
- Layout and font baselines not final; subject to change.
- Delta vertical centering uses a small baseline fix.
- SimHub CSV sender must be configured manually.

---

## License
MIT
