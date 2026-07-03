#pragma once
// CORE: active-low push button with short/long-press edge detection.
#include <Arduino.h>

enum ButtonEvent { BTN_NONE = 0, BTN_SHORT = 1, BTN_LONG = 2 };

class Button {
public:
  void begin(uint8_t pin) { _pin = pin; pinMode(pin, INPUT_PULLUP); }

  // Call every loop. Returns an event on release (debounced ~30ms).
  // longMs sets the short/long threshold.
  ButtonEvent poll(uint32_t longMs = 800) {
    bool level = digitalRead(_pin);
    ButtonEvent ev = BTN_NONE;
    if (_prev == HIGH && level == LOW) _tDown = millis();         // pressed
    if (_prev == LOW  && level == HIGH) {                          // released
      uint32_t held = millis() - _tDown;
      if (held > 30) ev = (held >= longMs) ? BTN_LONG : BTN_SHORT;
    }
    _prev = level;
    return ev;
  }

private:
  uint8_t  _pin   = 0;
  bool     _prev  = HIGH;
  uint32_t _tDown = 0;
};
