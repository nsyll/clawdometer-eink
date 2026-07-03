#pragma once
// ============================================================================
//  CORE (BLE): reusable NimBLE GATT-peripheral wrapper.
//  Advertises one service with a WRITE characteristic (central -> device) and
//  a NOTIFY characteristic (device -> central). App-agnostic.
//  Built for NimBLE-Arduino 1.4.x (Arduino-ESP32 2.0.x).
// ============================================================================
#include <Arduino.h>
#include <functional>
#include <string>

class Ble {
public:
  using RxHandler = std::function<void(const std::string&)>;

  // Init + advertise. `name` is the advertised device name (scanners match it).
  void begin(const char* name, const char* serviceUuid,
             const char* writeCharUuid, const char* notifyCharUuid);

  void onReceive(RxHandler h);        // called when the central writes the write-char
  bool connected() const;             // a central is connected
  void notify(const std::string& s);  // device -> central via the notify-char
  void readvertise();                 // (re)start advertising
};
