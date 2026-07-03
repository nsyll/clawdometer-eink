# Clawdometer — guide for AI coding agents

Read this before touching code. It encodes the hard-won gotchas; violating them produces firmware that compiles fine and then fails silently on the desk.

## What this is

An ESP32-S3 e-paper gadget showing Claude usage + Claude Code live status. Three parts:
`firmware/` (PlatformIO, Arduino, C++) · `daemon/` (single-file Python, bleak+httpx, macOS-first) · `hooks/` (Claude Code hook, stdlib-only Python). Interfaces between them are specced in `docs/PROTOCOL.md`; component responsibilities in `docs/ARCHITECTURE.md`.

## Build / run / test

```bash
# firmware
cd firmware && pio run                 # compile (always do this before claiming success)
./flash.sh                             # upload over USB — auto-picks the real serial port
pio device monitor                     # 115200 baud serial logs

# daemon (needs a real macOS Terminal for Bluetooth permission)
cd daemon && python3 -m venv .venv && ./.venv/bin/pip install -r requirements.txt
./.venv/bin/python claude_usage_daemon.py          # live dashboard
./.venv/bin/python claude_usage_daemon.py --plain  # line logs

# hook (test standalone; must ALWAYS exit 0)
echo '{"hook_event_name":"Stop","transcript_path":"<some.jsonl>"}' | python3 hooks/cc_status_hook.py
cat ~/.clawd/cc_status.json
```

There is no automated test suite. Verify firmware changes by compiling; verify daemon logic by importing the module and calling functions directly (it's import-safe); verify the hook with synthetic stdin payloads as above.

## Hardware gotchas (the expensive lessons)

1. **`EPD_PWR` (GPIO6) is ACTIVE-LOW.** `LOW` = panel powered. Get this backwards and the screen is just blank with no error.
2. **ES8311 codec needs MCLK before I2C.** I2S must be started before `es8311_init`, or the codec never ACKs. I2S clocks are stopped between chimes for power — `playMelody` restarts them.
3. **SPI pins must be remapped *after* `display.init()`** (see `Display.cpp`); the reverse order breaks the panel on this board.
4. **Deep sleep releases GPIO pads.** `VBAT_HOLD` (GPIO17) keeps the battery latch closed — it MUST be `gpio_hold_en`'d before `esp_deep_sleep_start()` or the board powers itself off and won't wake. Same technique holds the panel/audio rails off.
5. **BLE payload > default MTU.** The JSON is ~450 B; the device sets MTU 512. If you grow the payload past ~500 B, writes fail silently — shrink field names or split.
6. **NimBLE-Arduino must stay 1.4.x** while the platform is Arduino-ESP32 2.0.x (PlatformIO `espressif32`). 2.x NimBLE needs a newer core.

## Behavioral invariants (don't break these)

- **E-paper redraws are expensive and visible** (full-screen flash). Only redraw when the user would notice a change: cc *state*, `st`, `s`, `w`, or a button. Countdown/tool-name churn must NOT redraw; the 5-min heartbeat covers drift.
- **Chimes fire on transitions only** — a boot, reconnect, or repeated state must never beep. All chimes respect the persisted mute (`PWR` short press).
- **The hook must always `exit 0`** and never block — it runs inside Claude Code's turn. Heuristics go inside try/except.
- **Screen rendering lives only in `firmware/src/screen.cpp`** and is a pure function of `ClawdState`. `main.cpp` decides *when*, `screen.cpp` decides *what*.
- **After every flash, the daemon must be restarted** — flashing reboots the device and the daemon may hold a dead BLE handle. Tell the user this whenever you flash.
- The `question` state exists on top of `done`: `Stop` + final text ending in `?` (or Greek `;` in Greek text). Keep `shorten_question` output ≤34 chars — that's what fits the banner's small font.

## Conventions

- Firmware comments explain *constraints* (why the order matters, what breaks), not what the next line does. Match that.
- The daemon stays a **single file** — it's deliberately a read-once tool. Don't split it into a package.
- JSON payload field names are 1–3 chars (BLE size budget). Document any new field in `docs/PROTOCOL.md`.
- Battery/off-grid features: think in transitions and hysteresis (e.g. low-batt chime re-arms above 15%, shutdown needs two consecutive <3% reads, never on USB power).

## Things that look like bugs but aren't

- Device shows nothing after `pio run -t upload` → the daemon is holding a stale link; restart the daemon.
- `esptool` connecting to a Bluetooth audio device → macOS lists BT serial ports; always pin `--upload-port` (flash.sh does).
- Stats views blank right after boot → they fill on the first daemon push; the device intentionally renders only last-known data.
- `zZ` on screen and no BLE → deep sleep working as designed; any button wakes it.
