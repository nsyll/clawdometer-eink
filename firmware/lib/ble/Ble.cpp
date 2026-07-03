#include "Ble.h"
#include <NimBLEDevice.h>

// One peripheral per firmware -> file-scope state keeps the public API thin.
static NimBLEServer*         s_server     = nullptr;
static NimBLECharacteristic* s_notifyChar = nullptr;
static Ble::RxHandler        s_rx;
static volatile bool         s_connected  = false;

class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* srv, ble_gap_conn_desc* desc) override {
    s_connected = true;
    Serial.println("[BLE] connected");
    // Power: data flows about once a minute, but macOS defaults to a ~15ms
    // connection interval — the radio wakes ~67x/s for nothing. Request
    // 370-500ms (1.25ms units) with slave latency 3; Apple's accessory rule
    // interval*(latency+1) <= 2s holds (0.5s*4). Timeout 5s (10ms units).
    srv->updateConnParams(desc->conn_handle, 296, 400, 3, 500);
  }
  void onDisconnect(NimBLEServer*) override {
    s_connected = false;
    Serial.println("[BLE] disconnected -> re-advertising");
    NimBLEDevice::startAdvertising();
  }
};

class WriteCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    Serial.printf("[BLE] rx %u bytes\n", (unsigned)v.size());
    if (s_rx) s_rx(v);
  }
};

void Ble::begin(const char* name, const char* serviceUuid,
                const char* writeCharUuid, const char* notifyCharUuid) {
  NimBLEDevice::init(name);
  NimBLEDevice::setMTU(512);                 // fit the daemon's JSON (incl. usage stats) in one write

  s_server = NimBLEDevice::createServer();
  s_server->setCallbacks(new ServerCb());

  NimBLEService* svc = s_server->createService(serviceUuid);
  NimBLECharacteristic* wr = svc->createCharacteristic(
      writeCharUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  wr->setCallbacks(new WriteCb());
  s_notifyChar = svc->createCharacteristic(notifyCharUuid, NIMBLE_PROPERTY::NOTIFY);
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(svc->getUUID());
  adv->setScanResponse(true);                // carries the device name for name-based scans
  // Power: advertise every 0.8-1.0s (0.625ms units) instead of the fast default
  // burst — the daemon's 8s scans still catch several beacons per pass.
  adv->setMinInterval(1280);
  adv->setMaxInterval(1600);
  adv->start();
  Serial.printf("[BLE] advertising as \"%s\"\n", name);
}

void Ble::onReceive(RxHandler h) { s_rx = h; }
bool Ble::connected() const { return s_connected; }

void Ble::notify(const std::string& s) {
  if (s_notifyChar) { s_notifyChar->setValue(s); s_notifyChar->notify(); }
}

void Ble::readvertise() { NimBLEDevice::startAdvertising(); }
