#pragma once
// CORE: onboard sensors -- SHTC3 temp/humidity + battery voltage.

// Begin the I2C bus and the SHTC3 sensor. Returns true if the sensor responded.
bool sensorsBegin();

// Read indoor temperature (C) and relative humidity (%). Returns false on error.
bool readIndoor(float& tempC, float& humidity);

// Battery voltage (V) via ADC1_CH3 (GPIO4) with the board's /2 divider.
float batteryVolts();

// Battery charge estimate 0-100% (LiPo discharge curve from the voltage).
int batteryPercent();
