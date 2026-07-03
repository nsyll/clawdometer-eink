#pragma once
// ============================================================================
//  CORE: GPIO pin map for the Waveshare ESP32-S3-ePaper-1.54 board.
//  Verified against Waveshare's DEV_Config.h / user_config.h.
// ============================================================================

// e-paper (SSD1681) -- EPD_PWR is ACTIVE-LOW (drive LOW to power the panel)
#define EPD_PWR    6
#define EPD_BUSY   8
#define EPD_RST    9
#define EPD_DC     10
#define EPD_CS     11
#define EPD_SCK    12
#define EPD_MOSI   13

// I2C bus (SHTC3 temp/humidity, PCF85063 RTC, ES8311 codec all share it)
#define I2C_SDA    47
#define I2C_SCL    48

// Power management
#define VBAT_HOLD  17    // battery power-latch: HIGH keeps the board on off-USB
#define BATT_ADC   4     // battery voltage sense (ADC1_CH3), /2 divider

// Buttons (both ACTIVE-LOW with pull-up)
#define BTN_BOOT   0     // BOOT button
#define BTN_PWR    18    // PWR button (long hardware hold = power off)

// Audio (ES8311 codec + amplifier)
#define AUD_MCLK   14
#define AUD_BCLK   15
#define AUD_WS     38
#define AUD_DOUT   45
#define AUD_DIN    16
#define AUD_PA_EN  42    // codec power, ACTIVE-LOW (LOW = on)
#define AUD_PA_CTRL 46   // speaker amplifier enable, ACTIVE-HIGH
