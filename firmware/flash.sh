#!/bin/sh
# Flash Clawdometer over USB. Finds the board's serial port automatically.
set -e
PORT=$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1)
if [ -z "$PORT" ]; then
  echo "No board found. Plug the ESP32-S3 in with a USB *data* cable." >&2
  exit 1
fi
echo "Flashing via $PORT"
pio run -t upload --upload-port "$PORT"
echo
echo "Done. Remember: restart the daemon now (the flash rebooted the device"
echo "and dropped the BLE link)."
