# Clawdometer E-Ink 🦀

**A Claude Code usage monitor on an e-ink desk gadget** — rate-limit meters with reset countdowns, live WORKING / NEEDS YOU / QUESTION / DONE status, usage statistics, and a bell that rings the moment Claude Code **needs you**, **asks you a question**, or **finishes its work**.

Runs on a single off-the-shelf board — the **Waveshare ESP32-S3-ePaper-1.54** — with zero wiring and zero soldering. E-ink means it sits on your desk like a printed card: silent, always readable, about a week per battery charge.

<!-- photo: device on desk showing the meters -->

## What it does

- **Rate-limit meters** — your 5-hour and weekly Claude utilization as progress bars, with live reset countdowns. Never get surprised by a throttle again.
- **Claude Code status banner** — `WORKING` / `NEEDS YOU` / `QUESTION` / `DONE`, driven by Claude Code hooks in real time. When Claude ends its turn with a question, the question itself appears under the banner.
- **Distinct chimes** — a small sound language, one jingle per event, so your ears know what happened before your eyes do:

  | Event | Jingle |
  |---|---|
  | Needs you (permission / waiting) | knock-knock — two insistent beeps |
  | Asked you a question | rising "hm-hmm?" |
  | Finished working | ta-daa! (major arpeggio) |
  | Rate-limit hit (throttled) | low "uh-oh" |
  | Limit lifted | quick bright up-flick |
  | Battery low | slow quiet droop |

- **Usage statistics** — press BOOT to cycle: meters → **today** (tokens per hour) → **7 days** → **30 days** (per day), each with tokens, minutes Claude worked, minutes it waited on you, session and tool-call counts. Computed locally from Claude Code's own logs — fully backfilled, no cloud.
- **A week per charge** — deep sleep when nobody's around (the e-paper keeps its image at zero power, with a little "zZ"), relaxed BLE intervals, clock-gated audio, and a clean self-shutdown that protects the LiPo when truly empty.

## Hardware

One board, no wiring: the [Waveshare ESP32-S3-ePaper-1.54](https://www.waveshare.com/esp32-s3-epaper-1.54.htm) — ESP32-S3 (N8R8), 1.54-inch black/white 200×200 e-ink panel (SSD1681 controller, driven with GxEPD2), ES8311 audio codec + speaker, battery charge/latch circuit, and two buttons. Add any small LiPo if you want it cordless.

Beyond Clawdometer itself, the firmware doubles as a working reference for this board: GxEPD2/SSD1681 rendering with partial refresh and hibernate, ES8311 chime playback over I2S (with the MCLK-before-I2C init gotcha handled), NimBLE peripheral with relaxed connection parameters, deep sleep with the battery-latch pad held, and battery gauging off the ADC — see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## How it works

```
Claude Code ──hooks──▶ ~/.clawd/cc_status.json ─┐
                                                ├──▶ Mac daemon ──BLE──▶ ESP32-S3 e-paper
Anthropic API rate-limit headers ───────────────┤        (Python)
~/.claude/projects/**/*.jsonl (usage stats) ────┘
```

The daemon polls the Anthropic API's rate-limit headers (using your existing Claude Code OAuth token from the macOS Keychain — nothing new to configure), reads the hook-written status file, computes usage stats from Claude Code's local session logs, and pushes one small JSON payload to the device over BLE every minute. See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the exact payload.

## Setup

### 1. Flash the firmware

Requires [PlatformIO](https://platformio.org) (`pip install platformio` or the VS Code extension).

```bash
cd firmware
./flash.sh          # or: pio run -t upload
```

Plug the board in with a USB **data** cable. The device boots to "waiting for Mac...".

### 2. Install the daemon (macOS)

```bash
cd daemon
python3 -m venv .venv
./.venv/bin/pip install -r requirements.txt
./.venv/bin/python claude_usage_daemon.py
```

Run it from a real **Terminal** window — macOS grants Bluetooth permission per app, and the first run will prompt for it. You'll get a live dashboard (device link, meters, stats, recent events). Add `--plain` for classic line logging.

The daemon reads your Claude Code OAuth token from the Keychain. If you see `API HTTP 401`, refresh it with `claude setup-token`.

### 3. Register the Claude Code hooks (optional but the fun part)

This is what powers the WORKING / NEEDS YOU / QUESTION / DONE banner and chimes.

```bash
mkdir -p ~/.clawd
cp hooks/cc_status_hook.py ~/.clawd/
chmod +x ~/.clawd/cc_status_hook.py
```

Then merge `hooks/settings.example.json` into your `~/.claude/settings.json`. Hooks load when a Claude Code session starts, so restart any open sessions.

## Using it

| Control | Action |
|---|---|
| **BOOT** short press | cycle screens: meters → today → 7 days → 30 days |
| **PWR** short press | mute/unmute all chimes (persisted; mute icon in header) |
| **PWR** long press | re-advertise BLE (if the Mac lost the device) |
| any button while asleep | instant wake |

**Sleep**: after 5 minutes with no Mac (or 20 minutes of daemon silence) the device draws a small "zZ", keeps the last screen visible, and deep-sleeps at ~20 µA. It peeks for the daemon every 30 minutes and wakes instantly on any button.

**Battery**: shown in the header. Below 10% you get one quiet warning chime; below 3% (on battery) it draws "BATTERY EMPTY — charge me!" and powers itself off cleanly to protect the LiPo.

## Troubleshooting

- **Device shows stale data after re-flashing** → restart the daemon. Flashing reboots the device, which drops the BLE link; the daemon reconnects on its next scan but a long-running one can hold a dead handle.
- **`API HTTP 401`** → OAuth token expired: run `claude setup-token`, restart the daemon.
- **Daemon can't use Bluetooth** → it must run from an app with Bluetooth permission (a real Terminal, not an IDE-embedded shell). Check System Settings → Privacy & Security → Bluetooth.
- **`esptool` picks a wrong serial port** (e.g. Bluetooth headphones) → use `firmware/flash.sh`, which pins the real board port.
- **No banner while Claude works** → hooks load at session start; restart your Claude Code session after installing them.

## Privacy

Everything is local: the device talks only to your Mac over BLE; the daemon talks only to `api.anthropic.com` (a 1-token request whose *response headers* carry your rate-limit numbers) and reads Claude Code's own local logs. Nothing is uploaded anywhere.

## Project layout

```
firmware/   PlatformIO project (ESP32-S3, Arduino) — see docs/ARCHITECTURE.md
daemon/     single-file Python daemon (bleak + httpx)
hooks/      Claude Code hook + example settings.json registration
docs/       architecture & BLE/JSON protocol reference
```

## Acknowledgements

- **[Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter) by Hermann Björgvin** — the project that started it all. Clawdmeter (the animated AMOLED desk dashboard) was the inspiration and interaction reference for this build, and this project's daemon began life as a macOS/Python port of Clawdmeter's `claude-usage-daemon.sh` before growing its own features (Claude Code status, usage statistics, terminal dashboard). The name *Clawdometer* is a deliberate homage. If you prefer a colorful animated dashboard over e-ink, go check it out — it's excellent.
- The [Waveshare ESP32-S3-ePaper-1.54](https://www.waveshare.com/esp32-s3-epaper-1.54.htm) board, and the libraries that carry this project: [GxEPD2](https://github.com/ZinggJM/GxEPD2) (Jean-Marc Zingg), [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino), [ArduinoJson](https://arduinojson.org), [Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library), and [bleak](https://github.com/hbldh/bleak).
- Built almost entirely with [Claude Code](https://claude.com/claude-code) — which felt appropriate, given what it measures.

MIT licensed.
