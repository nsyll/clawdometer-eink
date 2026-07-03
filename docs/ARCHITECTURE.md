# Architecture

Three cooperating pieces, each replaceable on its own:

```
┌────────────────────────── Mac ──────────────────────────┐      ┌── ESP32-S3 ──┐
│                                                         │      │              │
│  Claude Code ──hooks──▶ ~/.clawd/cc_status.json         │      │  main.cpp    │
│                              │                          │ BLE  │  screen.cpp  │
│  Anthropic API headers ──┐   ├──▶ claude_usage_daemon ──┼─────▶│  lib/core    │
│  ~/.claude/projects logs ┴───┘        (Python)          │      │  lib/ble     │
└─────────────────────────────────────────────────────────┘      └──────────────┘
```

## Firmware (`firmware/`)

Two layers, deliberately kept apart:

- **`lib/core`** — board support, app-agnostic. One file per concern:
  - `board_pins.h` — the single source of truth for GPIO numbers. **`EPD_PWR` (GPIO6) is ACTIVE-LOW** — the classic gotcha on this board.
  - `BoardPower` — battery latch (`VBAT_HOLD`), panel rail, CPU clock, USB detection, light sleep helper.
  - `Display` — GxEPD2 instance + init (SPI pin remap must happen *after* `display.init`).
  - `Buttons` — debounced short/long-press polling.
  - `Sensors` — I2C bring-up (shared bus: SHTC3, RTC, ES8311) + battery ADC with a LiPo discharge-curve lookup.
  - `Audio` / `es8311` — I2S + codec. I2S must start **before** codec I2C init (the ES8311 won't ACK without MCLK). Clocks are stopped between chimes; each jingle starts/stops them.
- **`lib/ble`** — a thin NimBLE peripheral wrapper: one service, a write characteristic (daemon → device JSON) and a notify characteristic (device → daemon refresh nudge). Requests relaxed connection parameters on connect (power).

- **`src/`** — the Clawdometer app itself:
  - `main.cpp` — state (`ClawdState`), BLE JSON parsing, chime/redraw policy, buttons, deep-sleep controller.
  - `screen.cpp` — all rendering (meters view, stats views, banners, badges). Pure function of `ClawdState`.
  - `reset_font.h` — generated ~10px GFX font for small numerals/text.

### Design rules the code follows

1. **The screen is a pure function of `ClawdState`.** No rendering outside `screen.cpp`; `main.cpp` only decides *when* to redraw.
2. **Redraw only on meaningful change.** E-paper flashes on refresh, so pushes only mark dirty when the CC state, rate-limit status, or a utilization % changes — never on countdown/tool-name churn. A 5-minute heartbeat catches drift.
3. **Chimes fire on transitions, never on states.** Reconnects and boots are silent by construction.
4. **Sleep is the default.** No daemon for 5 min (or silence for 20) → render a "zZ" badge, hold the power-latch pad, deep-sleep. Buttons wake instantly; a 30-min timer wake sniffs 90 s for the daemon (covers its 60 s-capped retry backoff). The e-paper keeps the last image at zero power, so a sleeping device still "works" as a display.
5. **Everything respects the mute toggle**, which persists in NVS.

## Daemon (`daemon/claude_usage_daemon.py`)

Single file on purpose — it's a tool you read once and run forever. Sections:

- **Token**: read the Claude Code OAuth token from the macOS Keychain (or `~/.claude/.credentials.json` on Linux). Never stored.
- **Rate limits**: one minimal API request per minute; the numbers come from `anthropic-ratelimit-unified-*` response headers (works for Pro/Max; Enterprise spend-limit headers also handled).
- **CC status**: reads `~/.clawd/cc_status.json` (written by the hook), drops it when stale (>30 min).
- **Usage stats**: scans `~/.claude/projects/*/*.jsonl` for per-message token usage and timestamps; derives per-day/per-hour tokens, "Claude working" vs "your turn" minutes (message-gap heuristic, 10-min cap), session and tool counts. Cached 90 s.
- **BLE**: bleak; scan by name, write the JSON payload (~450 B, needs MTU > payload — the device requests 512).
- **UI**: ANSI dashboard when stdout is a TTY; `--plain` for logs.

## Hook (`hooks/cc_status_hook.py`)

Stdlib-only, exits 0 unconditionally (a hook must never break a Claude Code turn). Maps hook events to states:

- `UserPromptSubmit` / `PreToolUse` → `working` (message = tool name)
- `PreToolUse` of `AskUserQuestion` → `question` (message = the question)
- `Notification` → `needs-you` (message = reason)
- `Stop` → reads the transcript tail; if the final reply ends with a question mark (incl. Greek `;`) → `question`, else `done`

Writes `~/.clawd/cc_status.json` atomically.

## Why this shape

- **Device stays dumb.** It renders whatever state it's given and knows nothing about Anthropic, tokens, or hooks. New data = new JSON field + one render branch.
- **Daemon owns all policy** about *what* the numbers are; the device owns *when to redraw/beep* (it alone knows e-paper and speaker costs).
- **Hook is a dumb sensor.** One file, one output. Claude Code can evolve its hook payloads without touching device or daemon.
- The three interfaces between them (status file, JSON payload, GATT contract) are each a page of spec — see [PROTOCOL.md](PROTOCOL.md).
