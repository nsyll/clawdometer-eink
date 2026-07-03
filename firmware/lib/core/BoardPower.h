#pragma once
// CORE: board power management -- battery latch, panel rail, CPU, light sleep.
#include <esp_sleep.h>

// Latch battery power ON (so the board survives USB removal) and power the
// e-paper panel. Call this first in setup().
void boardPowerInit();

// Power the e-paper panel rail on/off (EPD_PWR is active-low).
void epdPower(bool on);

// Lower the CPU clock to save power (Wi-Fi still works at 80 MHz).
void cpuLowPower(uint32_t mhz = 80);

// True if attached to a USB host (SOF packets seen) -- i.e. cable plugged in.
// Used to skip sleep while on USB so flashing/debug always works.
bool usbPowered();

// Light-sleep for up to `ms`, also waking on a BOOT/PWR button press.
// RAM, GPIO state (incl. the VBAT latch) and millis() are all preserved.
// Returns the wake cause (ESP_SLEEP_WAKEUP_TIMER or ..._GPIO).
esp_sleep_wakeup_cause_t enterLightSleep(uint32_t ms);
