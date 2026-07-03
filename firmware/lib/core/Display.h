#pragma once
// CORE: e-paper display (SSD1681 200x200 B/W) init + shared object + helpers.
#include <GxEPD2_BW.h>

using EpdDisplay = GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT>;
extern EpdDisplay display;          // shared display object (app draws on it)

// Init the panel and remap SPI to this board's pins. Panel must be powered
// (call boardPowerInit / epdPower(true)) before this.
void displayBegin();

// Generic Wi-Fi glyph (a dot with expanding arcs), drawn at the given anchor.
void drawWifiIcon(int cx, int baseY);

// Battery glyph (outline + nub) with a fill bar for `pct` (0-100), top-left at x,y.
void drawBatteryIcon(int x, int y, int pct);
