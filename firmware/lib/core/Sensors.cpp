#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SHTC3.h>
#include "board_pins.h"
#include "Sensors.h"

static Adafruit_SHTC3 shtc3;

bool sensorsBegin() {
  Wire.begin(I2C_SDA, I2C_SCL);   // shared bus (SHTC3 + RTC + ES8311 codec)
  for (int i = 0; i < 3; i++) {   // retry: bus can be glitchy at cold boot
    if (shtc3.begin()) return true;
    delay(50);
  }
  Serial.println("[SHTC3] not found!");
  return false;
}

bool readIndoor(float& tempC, float& humidity) {
  sensors_event_t h, t;
  if (!shtc3.getEvent(&h, &t)) { Serial.println("[SHTC3] read failed"); return false; }
  tempC = t.temperature;
  humidity = h.relative_humidity;
  Serial.printf("[SHTC3] %.1f C  %.0f%%\n", tempC, humidity);
  return true;
}

float batteryVolts() { return analogReadMilliVolts(BATT_ADC) * 2.0f / 1000.0f; }

int batteryPercent() {
  float v = batteryVolts();
  // LiPo discharge curve (voltage -> %); interpolated, not naive-linear.
  static const float vt[] = {3.30f,3.50f,3.60f,3.70f,3.75f,3.80f,3.85f,3.90f,3.95f,4.00f,4.10f,4.20f};
  static const int   pt[] = {   0,    5,   12,   25,   35,   48,   58,   68,   78,   85,   94,  100};
  const int n = sizeof(vt) / sizeof(vt[0]);
  if (v <= vt[0])   return 0;
  if (v >= vt[n-1]) return 100;
  for (int i = 1; i < n; i++) {
    if (v < vt[i]) {
      float f = (v - vt[i-1]) / (vt[i] - vt[i-1]);
      return (int)lroundf(pt[i-1] + f * (pt[i] - pt[i-1]));
    }
  }
  return 100;
}
