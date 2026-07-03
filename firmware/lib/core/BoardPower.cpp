#include <Arduino.h>
#include "driver/gpio.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"
#include "board_pins.h"
#include "BoardPower.h"

bool usbPowered() {
  // The USB-Serial-JTAG SOF frame index only advances while attached to a host.
  uint32_t a = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & USB_SERIAL_JTAG_SOF_FRAME_INDEX;
  delay(3);   // a few 1ms USB frames
  uint32_t b = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & USB_SERIAL_JTAG_SOF_FRAME_INDEX;
  return a != b;
}

void epdPower(bool on) {
  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, on ? LOW : HIGH);   // ACTIVE-LOW: LOW = panel ON
}

void cpuLowPower(uint32_t mhz) {
  setCpuFrequencyMhz(mhz);
  Serial.printf("[Power] CPU @ %lu MHz\n", (unsigned long)getCpuFrequencyMhz());
}

esp_sleep_wakeup_cause_t enterLightSleep(uint32_t ms) {
  if (ms < 1000) ms = 1000;
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
  // wake on either button going LOW (both are active-low with pull-ups)
  gpio_wakeup_enable((gpio_num_t)BTN_BOOT, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)BTN_PWR,  GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  Serial.flush();
  esp_light_sleep_start();
  return esp_sleep_get_wakeup_cause();
}

void boardPowerInit() {
  // Latch battery power ON first (needs a battery attached; harmless on USB).
  pinMode(VBAT_HOLD, OUTPUT);
  digitalWrite(VBAT_HOLD, HIGH);

  epdPower(true);
  delay(50);
}
