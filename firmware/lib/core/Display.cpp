#include <Arduino.h>
#include <SPI.h>
#include "board_pins.h"
#include "Display.h"

EpdDisplay display(GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

void displayBegin() {
  // init first, then remap SPI to this board's pins (required order on ESP32)
  display.init(115200, true, 2, false);
  SPI.end();
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
}

// One upward-opening arc of the Wi-Fi symbol, centered on the dot
static void drawWifiArc(int cx, int cy, int r) {
  for (float a = -2.356f; a <= -0.785f; a += 0.05f) {   // -135deg .. -45deg
    int x = cx + (int)lround(r * cos(a));
    int y = cy + (int)lround(r * sin(a));
    display.drawPixel(x, y, GxEPD_BLACK);
    display.drawPixel(x, y - 1, GxEPD_BLACK);            // 2px thickness
  }
}

void drawWifiIcon(int cx, int baseY) {
  display.fillCircle(cx, baseY, 1, GxEPD_BLACK);
  drawWifiArc(cx, baseY, 4);
  drawWifiArc(cx, baseY, 8);
  drawWifiArc(cx, baseY, 12);
}

void drawBatteryIcon(int x, int y, int pct) {
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  const int w = 18, h = 9;
  display.drawRect(x, y, w, h, GxEPD_BLACK);          // body
  display.fillRect(x + w, y + 3, 2, 3, GxEPD_BLACK);  // + terminal nub
  int fill = (pct * (w - 4)) / 100;                   // inner fill bar
  if (fill > 0) display.fillRect(x + 2, y + 2, fill, h - 4, GxEPD_BLACK);
}
