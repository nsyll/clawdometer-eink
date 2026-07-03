#!/usr/bin/env python3
"""Clawdometer daemon (BLE, macOS).

Began as a macOS/Python port of the claude-usage-daemon.sh from Hermann
Björgvin's Clawdmeter (https://github.com/HermannBjorgvin/Clawdmeter) —
kudos! — and has since grown Claude Code status forwarding, local usage
statistics, and a live terminal dashboard.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Clawdometer" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

import asyncio
import calendar
import collections
import datetime
import getpass
import glob
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

DEVICE_NAME = "Clawdometer"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
SCAN_TIMEOUT = 8.0
CONNECT_TIMEOUT = 20.0

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
DEFAULT_CONFIG_DIR = Path.home() / ".claude"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
CONFIG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "config"

# Claude Code interaction status, written by the CC hook (~/.clawd/cc_status_hook.py).
CC_STATUS_FILE = Path.home() / ".clawd" / "cc_status.json"
CC_STALE_SECS = 30 * 60  # drop the banner if CC has been silent this long

# Local Claude Code usage stats, derived from the transcript logs (backfilled).
CC_LOG_GLOB = str(Path.home() / ".claude" / "projects" / "*" / "*.jsonl")
HISTORY_DAYS = 20
GAP_CAP_SECS = 600          # gaps longer than this count as "away", not active
_stats_cache = None         # (computed_at, result) — recomputed at most every 90s

API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}


# ---------------------------------------------------------------------------
# Terminal dashboard: a live status panel instead of scrolling JSON. Active
# when stdout is a real terminal; pass --plain (or pipe the output) to get the
# classic line-by-line log. Zero dependencies — plain ANSI.
# ---------------------------------------------------------------------------
_ANSI = {"reset": "\x1b[0m", "bold": "\x1b[1m", "dim": "\x1b[2m",
         "red": "\x1b[31m", "green": "\x1b[32m", "yellow": "\x1b[33m",
         "cyan": "\x1b[36m"}


def _fmt_tokens(k: int) -> str:            # value is in thousands
    if k >= 1000:
        return f"{k / 1000:.1f}M"
    return f"{k}K" if k > 0 else "0"


def _fmt_minutes(m: int) -> str:
    return f"{m / 60:.1f}h" if m >= 90 else f"{m}m"


def _fmt_reset_mins(mins: int) -> str:
    if mins is None or mins < 0:
        return "--"
    if mins >= 1440:
        return f"{mins // 1440}d{(mins % 1440) // 60}h"
    if mins >= 60:
        return f"{mins // 60}h{mins % 60:02d}"
    return f"{mins}m"


def _bar(pct: int, width: int = 22) -> str:
    pct = max(0, min(100, pct if pct is not None else 0))
    fill = round(pct * width / 100)
    return "█" * fill + "░" * (width - fill)


class Dashboard:
    def __init__(self) -> None:
        self.enabled = (sys.stdout.isatty()
                        and "--plain" not in sys.argv
                        and os.environ.get("TERM", "") != "dumb")
        self.events: collections.deque = collections.deque(maxlen=7)
        self.payload: dict | None = None
        self.push_ts: float = 0.0
        self.push_ok: bool = True
        self.push_size: int = 0
        self.linked = False
        self._painted = False
        if self.enabled:
            import atexit
            atexit.register(lambda: sys.stdout.write("\x1b[?25h\x1b[0m\n"))

    def event(self, msg: str) -> None:
        # infer the link state from the messages the call sites already log
        if msg == "Connected":
            self.linked = True
        elif msg.startswith(("Device disconnected", "Device not found", "Stopping")):
            self.linked = False
        self.events.appendleft(f"{time.strftime('%H:%M:%S')}  {msg}")
        self.render()

    def push(self, payload: dict, size: int, ok: bool) -> None:
        self.payload = payload
        self.push_ts = time.time()
        self.push_ok = ok
        self.push_size = size
        if not ok:
            self.events.appendleft(f"{time.strftime('%H:%M:%S')}  push FAILED")
        self.render()

    def render(self) -> None:
        if not self.enabled:
            return
        A = _ANSI
        p = self.payload or {}
        W = 66
        rule = A["dim"] + "─" * W + A["reset"]
        lines = []
        lines.append(A["bold"] + " CLAWDOMETER daemon" + A["reset"]
                     + f"{time.strftime('%H:%M:%S'):>{W - 18}}")
        lines.append(rule)

        vis = "● linked" if self.linked else "○ searching"
        col = A["green"] if self.linked else A["red"]
        push = (time.strftime("%H:%M:%S", time.localtime(self.push_ts))
                if self.push_ts else "--")
        pmark = "" if self.push_ok else A["red"] + " (write failed!)" + A["reset"]
        lines.append(f" Device      {col}{vis}{A['reset']}{' ' * max(1, 14 - len(vis))}"
                     f"last push {push} ({self.push_size}B){pmark}")

        cc = str(p.get("cc", "") or "")
        ccm = str(p.get("ccm", "") or "")
        cc_col = {"working": A["cyan"], "needs-you": A["red"],
                  "question": A["yellow"], "done": A["green"]}.get(cc, "")
        cc_disp = (cc_col + cc.upper().replace("-", " ") + A["reset"]
                   + (f"  {A['dim']}{ccm}{A['reset']}" if ccm else "")) if cc else A["dim"] + "--" + A["reset"]
        lines.append(f" Claude Code {cc_disp}")
        lines.append(rule)

        s, w = p.get("s"), p.get("w")
        lines.append(f" Session 5h {s if s is not None else '--':>4}%"
                     f"  {_bar(s)}  resets {_fmt_reset_mins(p.get('sr', -1))}")
        lines.append(f" Weekly 7d  {w if w is not None else '--':>4}%"
                     f"  {_bar(w)}  resets {_fmt_reset_mins(p.get('wr', -1))}")
        lines.append(rule)

        lines.append(A["dim"] + f" {'':10}{'tokens':>8}{'working':>10}"
                     f"{'your turn':>11}{'sessions':>10}{'tools':>8}" + A["reset"])
        for label, key in (("today", "d"), ("7 days", "d7"), ("30 days", "d30")):
            d = p.get(key) or {}
            lines.append(f" {label:<10}{_fmt_tokens(d.get('tk', 0)):>8}"
                         f"{_fmt_minutes(d.get('wk', 0)):>10}"
                         f"{_fmt_minutes(d.get('yt', 0)):>11}"
                         f"{d.get('se', 0):>10}{d.get('tl', 0):>8}")
        lines.append(rule)
        for ev in self.events:
            lines.append(A["dim"] + " " + ev + A["reset"])

        out = "\x1b[?25l"
        out += "\x1b[2J\x1b[H" if not self._painted else "\x1b[H"
        self._painted = True
        out += "".join(ln + "\x1b[K\n" for ln in lines) + "\x1b[J"
        sys.stdout.write(out)
        sys.stdout.flush()


DASH = Dashboard()


def log(msg: str) -> None:
    if DASH.enabled:
        DASH.event(msg)
    else:
        print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _read_token_keychain() -> str | None:
    try:
        out = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                getpass.getuser(),
                "-w",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(out.stdout)


def read_config_dirs() -> list[Path]:
    """Claude config dirs to poll, from the `config_dirs` option (comma list).

    Defaults to [~/.claude] so existing single-plan setups are unchanged. ~ is
    expanded. Mirrors the Linux bash daemon's read_config_dirs.
    """
    raw = ""
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "config_dirs":
                    raw = val.strip()
    except OSError:
        pass
    if not raw:
        return [DEFAULT_CONFIG_DIR]
    dirs = [Path(p.strip()).expanduser() for p in raw.split(",") if p.strip()]
    return dirs or [DEFAULT_CONFIG_DIR]


def read_token_for(config_dir: Path) -> str | None:
    """Read the OAuth token for one config dir.

    Linux: each dir keeps its own ``<dir>/.credentials.json``. macOS: the default
    install stores the token in Keychain with no file, so for the default dir we
    fall back to Keychain when no file is present — preserving existing
    single-plan macOS behavior. Additional macOS dirs are read from their files;
    a work plan whose token lives only in the single Keychain entry can't be told
    apart there (documented follow-up).
    """
    cred = config_dir / ".credentials.json"
    try:
        if cred.exists():
            return _extract_access_token(cred.read_text())
    except OSError as e:
        log(f"Error reading credentials in {config_dir}: {e}")
    if sys.platform == "darwin" and config_dir == DEFAULT_CONFIG_DIR:
        return _read_token_keychain()
    return None


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


def save_address(addr: str) -> None:
    SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR_FILE.write_text(addr)


async def scan_for_device() -> str | None:
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d.address
    return None


# --- macOS: recover a device the OS already holds as an HID keyboard --------
#
# The firmware advertises as a BLE HID keyboard so its buttons type into the
# Mac. macOS auto-connects to that HID, and CoreBluetooth then EXCLUDES the
# peripheral from BleakScanner.discover() results (already-connected devices
# never appear in scans). bleak's connect-by-address path also scans
# internally, so a cached address can't help either. The documented escape
# hatch is retrieveConnectedPeripheralsWithServices_, which returns
# peripherals the system is already connected to. We wrap the result in a
# BLEDevice carrying the live (peripheral, manager) details so BleakClient
# connects to it directly without scanning. CoreBluetooth shares the single
# physical link, so this rides the existing HID connection — the keyboard
# keeps working.
_cb_manager = None  # reused CentralManagerDelegate (CoreBluetooth)


async def _get_cb_manager():
    """Lazily create and ready a shared CoreBluetooth central manager."""
    global _cb_manager
    if _cb_manager is None:
        from bleak.backends.corebluetooth.CentralManagerDelegate import (
            CentralManagerDelegate,
        )

        mgr = CentralManagerDelegate()
        await mgr.wait_until_ready()  # raises if Bluetooth is unauthorized/off
        _cb_manager = mgr
    return _cb_manager


async def retrieve_connected_macos(skip_addr: str | None = None):
    """Return a BLEDevice for a system-connected 'Clawdometer', or None.

    Two-step lookup, strongest signal first:

    1. Peripherals connected under our CUSTOM service UUID. Membership in
       that service is unambiguous (no other device exposes it), so we accept
       by service alone — the peripheral's name can be None on macOS.
    2. Fall back to the generic HID service 0x1812, but ONLY trust a
       peripheral whose name matches DEVICE_NAME. 0x1812 also matches
       unrelated keyboards/mice, so picking blindly here could grab the
       wrong device.

    ``skip_addr`` skips a peripheral whose UUID just failed to connect, so a
    stale CoreBluetooth handle can't trap us into never trying a fresh scan.
    """
    from CoreBluetooth import CBUUID
    from bleak.backends.device import BLEDevice

    try:
        manager = await _get_cb_manager()
    except Exception as e:  # BleakBluetoothNotAvailableError etc.
        log(f"CoreBluetooth unavailable: {e}")
        return None

    cm = manager.central_manager

    def _wrap(p):
        addr = p.identifier().UUIDString()
        log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
        return BLEDevice(addr, p.name(), (p, manager))

    def _ok(p) -> bool:
        return not (skip_addr and p.identifier().UUIDString() == skip_addr)

    # 1. Custom service — accept by service membership alone.
    custom = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )
    for p in custom or []:
        if _ok(p):
            return _wrap(p)

    # 2. Generic HID service — require an exact name match.
    hid = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_("1812")]
    )
    for p in hid or []:
        if _ok(p) and p.name() == DEVICE_NAME:
            return _wrap(p)

    return None


async def discover_target(skip_addr: str | None = None):
    """Return a connectable target, or None.

    macOS: prefer the system-connected peripheral (HID-grabbed devices are
    invisible to scans); fall back to a normal scan that yields a BLEDevice
    so the subsequent connect doesn't have to re-scan. ``skip_addr`` is
    forwarded so a just-failed peripheral is skipped, making the scan
    fallback reachable.

    Other platforms: keep the original cached-address / scan-by-name flow.
    A freshly scanned address is cached here (the only place it's saved).
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is not None:
            return dev
        log(f"Not held by OS; scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
        dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT)
        if dev:
            log(f"Found: {dev.address}")
        return dev

    address = load_cached_address()
    if not address:
        address = await scan_for_device()
        if address:
            save_address(address)  # cache only freshly-scanned addresses
    return address


def read_chime_setting() -> str:
    """Read the `chime` option from the config file. One of: off|on.

    Defaults to "off" (the device stays silent) so existing setups are
    unaffected until the user opts in.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "chime":
                    val = val.strip().lower()
                    if val in ("off", "on"):
                        return val
    except OSError:
        pass
    return "off"


def read_clock_setting() -> str:
    """Read the `clock` option from the config file. One of: off|auto|12|24.

    Defaults to "off" (no clock; the device keeps showing "Usage") so existing
    setups are unaffected until the user opts in.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "clock":
                    val = val.strip().lower()
                    if val in ("off", "auto", "12", "24"):
                        return val
    except OSError:
        pass
    return "off"


def add_chime_field(payload: dict) -> None:
    """Add "c":1 to the payload when the config opts in, so the firmware may
    sound the session-reset chime. Omitted entirely when chime is off."""
    if read_chime_setting() == "on":
        payload["c"] = 1


def detect_hour_format() -> int:
    """Best-effort 12h/24h detection for the host. Returns 12 or 24 (default 24)."""
    # macOS: the explicit System Settings toggle lives in NSGlobalDomain.
    for key, result in (("AppleICUForce24HourTime", 24), ("AppleICUForce12HourTime", 12)):
        try:
            out = subprocess.run(["defaults", "read", "-g", key],
                                 capture_output=True, text=True, timeout=3)
            if out.stdout.strip() == "1":
                return result
        except (OSError, subprocess.SubprocessError):
            pass
    # Fallback to the C locale's time format (may be C/24h under launchd).
    try:
        import locale
        locale.setlocale(locale.LC_TIME, "")
        fmt = locale.nl_langinfo(locale.T_FMT)
        if "%p" in fmt or "%r" in fmt or "%I" in fmt:
            return 12
    except (ImportError, locale.Error, AttributeError):
        pass
    return 24


def add_clock_fields(payload: dict) -> None:
    """Add wall-clock fields to the payload when the config opts in.

    "t"  = local wall-clock epoch (UTC epoch shifted by the tz offset) so the
           device can show the time without an RTC.
    "tf" = 12 or 24, the hour format the device should render.
    """
    clock = read_clock_setting()
    if clock == "off":
        return
    tf = 24 if clock == "24" else 12 if clock == "12" else detect_hour_format()
    payload["t"] = int(time.time()) + time.localtime().tm_gmtoff
    payload["tf"] = tf


def read_cc_status() -> dict:
    """Read the Claude Code interaction status written by the CC hooks.

    Returns the BLE-payload fields to merge — {"cc","ccm","cct"} — or {} when
    the file is absent, malformed, or older than CC_STALE_SECS (so a crashed or
    long-idle CC session doesn't pin a stale banner on the device).
    "cc"  = state (working|needs-you|done), "ccm" = short message, "cct" = ts.
    """
    try:
        d = json.loads(CC_STATUS_FILE.read_text())
    except (OSError, ValueError, TypeError):
        return {}
    state = str(d.get("state", "")).strip()
    if not state:
        return {}
    try:
        ts = int(d.get("ts", 0))
    except (ValueError, TypeError):
        ts = 0
    if ts and (time.time() - ts) > CC_STALE_SECS:
        return {}
    out = {"cc": state[:12], "cct": ts}
    msg = str(d.get("message", "")).strip()
    if msg:
        out["ccm"] = msg[:40]  # keep short for the 200x200 panel
    return out


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
    if resp.status_code >= 400:
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts: str) -> int:
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util: str) -> int:
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    # Pro/Max accounts expose 5h/7d windows; Enterprise/overage use a single
    # spending-limit model reported via overage-utilization.
    if resp.headers.get("anthropic-ratelimit-unified-5h-utilization"):
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
            "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
            "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
            "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
            "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
            "acct": "pro",
            "ok": True,
        }
    else:
        reset_ts = hdr("anthropic-ratelimit-unified-overage-reset")
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-overage-utilization")),
            "sr": reset_minutes(reset_ts),
            "w": 0,
            "wr": 0,
            "st": hdr("anthropic-ratelimit-unified-status", "unknown"),
            "acct": "ent",
            **_billing_period_info(now, reset_ts),
            "ok": True,
        }
    add_chime_field(payload)   # adds "c":1 iff the config opts in
    add_clock_fields(payload)   # adds "t" + "tf" iff the config opts in
    return payload


def _billing_period_info(now: float, reset_ts: str) -> dict:
    """Fraction of billing period elapsed (tp, 0-100) and period length in days (pd).

    Billing periods are assumed calendar-monthly: period_end is the reset
    timestamp, period_start is the same day/time one calendar month earlier.

    The rate-limit headers expose only the reset timestamp, not the period
    length, so the monthly window is an assumption — but a documented one:
    Enterprise spend-limit `period` "the only value today is monthly"
    (Claude Enterprise Admin API reference). The doc notes period is an open
    string that may gain other values later; revisit this if so.
    """
    try:
        period_end = float(reset_ts)
    except ValueError:
        return {"tp": 0, "pd": 30}
    dt_end = datetime.datetime.fromtimestamp(period_end)
    prev_month = dt_end.month - 1 or 12
    prev_year = dt_end.year if dt_end.month > 1 else dt_end.year - 1
    prev_day = min(dt_end.day, calendar.monthrange(prev_year, prev_month)[1])
    dt_start = dt_end.replace(year=prev_year, month=prev_month, day=prev_day)
    period_start = dt_start.timestamp()
    period_len = period_end - period_start
    if period_len <= 0:
        return {"tp": 0, "pd": 30}
    pct_val = (now - period_start) / period_len * 100
    total_days = int(round(period_len / 86400))
    rd = f"{dt_end.strftime('%b')} {dt_end.day}"
    return {
        "tp": max(0, min(100, int(round(pct_val)))),
        "pd": total_days,
        "rd": rd,
    }


class PlanSelector:
    """Decide which config dir's plan is "active" across polls.

    "Active" = the plan whose session % rose most recently (recent API activity).
    A rise stamps a monotonic poll counter, so the choice is sticky and a window
    reset (a drop to 0) isn't mistaken for use. Before any rise is seen (startup)
    the highest current session % wins. Mirrors the Linux bash daemon.
    """

    def __init__(self) -> None:
        self.prev_s: dict[Path, int] = {}
        self.last_active: dict[Path, int] = {}
        self.seq = 0

    def choose(self, sessions: dict[Path, int]) -> Path:
        """Update state from this cycle's {dir: session_pct} and return the active dir."""
        self.seq += 1
        for d, s in sessions.items():
            if d in self.prev_s and s > self.prev_s[d]:
                self.last_active[d] = self.seq
            self.prev_s[d] = s
        # Most recent activity wins; ties (and the startup case) break by highest %.
        return max(sessions, key=lambda d: (self.last_active.get(d, 0), sessions[d]))


# Module-level so the active-plan state survives reconnects.
_SELECTOR = PlanSelector()


def compute_usage_stats() -> dict:
    """Read the local Claude Code transcripts and return usage stats (backfilled).

    Returns (tokens compacted to thousands to fit the BLE MTU):
      "hh":  [24] today's tokens/1000 per hour-of-day
      "h":   [30] daily tokens/1000, oldest..newest
      "d"/"d7"/"d30": {tk,wk,yt,se,tl} for last 1 / 7 / 30 days — tokens/1000,
                      Claude-working minutes, your-turn minutes, sessions, tool calls
    Cached 90s so frequent BLE pushes don't re-parse the logs.
    """
    global _stats_cache
    now = time.time()
    if _stats_cache and now - _stats_cache[0] < 90:
        return _stats_cache[1]

    tokens = collections.defaultdict(int)     # day -> input+output tokens
    working = collections.defaultdict(float)  # day -> seconds (user->assistant)
    yourturn = collections.defaultdict(float) # day -> seconds (assistant->user)
    sessions = collections.defaultdict(set)   # day -> {session ids}
    tools = collections.defaultdict(int)      # day -> tool_use count
    today = datetime.date.today()
    hourly = [0] * 24                          # today's tokens per hour-of-day

    for f in glob.glob(CC_LOG_GLOB):
        sid = os.path.basename(f)[:8]
        events = []  # (datetime, role) within this session, for gap classification
        try:
            fh = open(f, errors="ignore")
        except OSError:
            continue
        with fh:
            for ln in fh:
                if '"timestamp"' not in ln:
                    continue
                try:
                    rec = json.loads(ln)
                except (ValueError, TypeError):
                    continue
                ts = rec.get("timestamp", "")
                if len(ts) < 19:
                    continue
                try:
                    t = datetime.datetime.fromisoformat(ts.replace("Z", "+00:00")).astimezone()
                except ValueError:
                    continue
                dd = t.date()
                day = dd.isoformat()
                msg = rec.get("message") or {}
                u = msg.get("usage") or {}
                tok = (int(u.get("input_tokens", 0)) + int(u.get("output_tokens", 0))) if u else 0
                if tok:
                    tokens[day] += tok
                    if dd == today:
                        hourly[t.hour] += tok
                role = msg.get("role")
                if role:
                    sessions[day].add(sid)
                    events.append((t, role))
                for b in (msg.get("content") or []):
                    if isinstance(b, dict) and b.get("type") == "tool_use":
                        tools[day] += 1
        events.sort(key=lambda e: e[0])
        for (ta, ra), (tb, rb) in zip(events, events[1:]):
            gap = (tb - ta).total_seconds()
            if gap >= GAP_CAP_SECS:
                continue
            day = ta.date().isoformat()
            if ra == "user" and rb == "assistant":
                working[day] += gap
            elif ra == "assistant" and rb == "user":
                yourturn[day] += gap

    def days_back(n):
        return [(today - datetime.timedelta(days=i)).isoformat() for i in range(n)]

    def summary(n):
        w = days_back(n)
        return {
            "tk": sum(tokens.get(d, 0) for d in w) // 1000,
            "wk": round(sum(working.get(d, 0) for d in w) / 60),
            "yt": round(sum(yourturn.get(d, 0) for d in w) / 60),
            "se": len(set().union(*[sessions.get(d, set()) for d in w])) if w else 0,
            "tl": sum(tools.get(d, 0) for d in w),
        }

    res = {
        "hh": [x // 1000 for x in hourly],
        "h": [tokens.get((today - datetime.timedelta(days=i)).isoformat(), 0) // 1000
              for i in range(29, -1, -1)],
        "d": summary(1),
        "d7": summary(7),
        "d30": summary(30),
    }
    _stats_cache = (now, res)
    return res


async def poll_active_payload(selector: PlanSelector = _SELECTOR) -> dict | None:
    """Poll every configured config dir and return the active plan's payload.

    Returns None when no dir yields a usable payload this cycle. A single
    configured dir (the default) collapses to exactly the old single-poll path.
    """
    dirs = read_config_dirs()
    payloads: dict[Path, dict] = {}
    sessions: dict[Path, int] = {}
    for d in dirs:
        token = read_token_for(d)
        if not token:
            log(f"No token in {d}; skipping")
            continue
        payload = await poll_api(token)
        if payload is not None:
            payloads[d] = payload
            sessions[d] = int(payload.get("s", 0) or 0)
    if not payloads:
        return None
    active = selector.choose(sessions)
    if len(dirs) > 1:
        log(f"Active plan: {active} (s={sessions[active]})")
    p = payloads[active]
    p.update(compute_usage_stats())    # local-log usage: hourly + 30-day + summaries
    return p


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        # start_notify awaits CoreBluetooth's CCCD-write confirmation, which
        # never arrives if the peripheral doesn't ACK the subscribe (a
        # half-open link after the OS auto-connects the HID). Unbounded, that
        # await wedges the whole daemon between "Connected" and the first poll
        # — the device then shows nothing until a manual restart. Bound it: the
        # subscription is only an optional device-initiated refresh nudge (we
        # poll every POLL_INTERVAL regardless), so on timeout we proceed.
        try:
            await asyncio.wait_for(
                self.client.start_notify(REQ_CHAR_UUID, self._on_refresh),
                timeout=10,
            )
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")
        except asyncio.TimeoutError:
            log("Refresh subscription timed out; polling without it")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        if not DASH.enabled:
            log(f"Sending: {data.decode()}")
        ok = True
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
        except BleakError as e:
            log(f"Write failed: {e}")
            ok = False
        if DASH.enabled:
            DASH.push(payload, len(data), ok)
        return ok


def _is_encryption_error(exc: BaseException) -> bool:
    """True if a connect error is a macOS bonding/encryption mismatch.

    macOS reports a stale bond as CBErrorDomain Code=15 ("Failed to encrypt
    the connection..."). Match on the message text so we don't depend on how
    bleak wraps the underlying CoreBluetooth error.
    """
    s = str(exc).lower()
    return "code=15" in s or "encrypt" in s


# blueutil talks to Bluetooth via IOBluetooth, which on recent macOS needs its
# OWN Bluetooth TCC grant (separate from the daemon's CoreBluetooth grant).
# Without it, blueutil *hangs* instead of erroring — so every call is bounded
# by a timeout and a hang is reported as a permission problem, not a crash.
BLUEUTIL_TIMEOUT = 8


def _blueutil(*args: str) -> str | None:
    """Run `blueutil <args>`, returning stdout, or None on failure/timeout.

    A timeout almost always means blueutil lacks Bluetooth permission (it
    blocks rather than failing), so we surface that cause explicitly.
    """
    try:
        return subprocess.run(
            ["blueutil", *args],
            capture_output=True, text=True,
            timeout=BLUEUTIL_TIMEOUT, check=True,
        ).stdout
    except subprocess.TimeoutExpired:
        log(f"blueutil {' '.join(args)} timed out — it likely lacks Bluetooth "
            "permission. Grant it under System Settings > Privacy & Security > "
            "Bluetooth (run `blueutil --paired` once from Terminal to prompt).")
        return None
    except (subprocess.SubprocessError, OSError) as e:
        log(f"blueutil {' '.join(args)} failed: {e}")
        return None


def unpair_macos() -> bool:
    """Forget a stale macOS bond for DEVICE_NAME so the device can re-pair.

    A Code=15 "failed to encrypt" connect error means macOS holds bonding
    keys that no longer match the ESP32's (e.g. after a firmware reflash or
    the on-device bond-clear gesture). The firmware pairs "just works" (no
    MITM), so once the stale bond is gone the next connect re-bonds silently
    with no GUI prompt.

    CoreBluetooth exposes no unpair API, so we shell out to `blueutil`. The
    daemon only knows the peripheral's CoreBluetooth UUID, not the BD_ADDR
    that blueutil needs, so we map by name via `blueutil --paired`. Returns
    True if a bond was removed. Mirrors the Linux daemon's `bluetoothctl
    remove` self-heal.
    """
    if not shutil.which("blueutil"):
        log("Stale bond detected but `blueutil` is not installed; cannot "
            "auto-recover. Run `brew install blueutil`, or forget "
            f"'{DEVICE_NAME}' in System Settings > Bluetooth and reconnect.")
        return False

    out = _blueutil("--paired")
    if out is None:
        return False

    # Each line looks like:
    #   address: 28-84-85-55-5c-3d, ... name: "Clawdometer", ...
    addr = None
    for line in out.splitlines():
        if f'name: "{DEVICE_NAME}"' in line:
            m = re.search(r"address:\s*([0-9a-fA-F:-]+)", line)
            if m:
                addr = m.group(1)
                break
    if not addr:
        log(f"No paired '{DEVICE_NAME}' found to unpair (already forgotten?)")
        return False

    if _blueutil("--unpair", addr) is None:
        return False
    log(f"Unpaired stale bond for '{DEVICE_NAME}' [{addr}]; re-pairing on "
        "next connect")
    return True


async def connect_and_run(target, stop_event: asyncio.Event) -> bool:
    """Connect to a target and poll until disconnected or stopped.

    ``target`` is either an address string (Linux) or a BLEDevice carrying
    live CoreBluetooth details (macOS). Returns True if the connection was
    used successfully (so the caller keeps the cached address), False if the
    connection failed and the cache should be invalidated.
    """
    display = target if isinstance(target, str) else target.address
    log(f"Connecting to {display}...")
    client = BleakClient(target)
    try:
        # Bound the connect the same way #84 bounded the refresh subscribe.
        # On macOS the OS auto-connects the firmware's HID link, so
        # CoreBluetooth can hand us a half-open peripheral whose GATT connect
        # handshake never completes. BleakClient's own timeout governs
        # discovery, not connectPeripheral, so an unbounded await here wedges
        # the single-threaded daemon forever at "Connecting..." (observed ~13h,
        # device stuck on stale data). wait_for raises TimeoutError, which the
        # handler below already treats as a connection failure -> drop the
        # cached address and rescan.
        await asyncio.wait_for(client.connect(), timeout=CONNECT_TIMEOUT)
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        if sys.platform == "darwin" and _is_encryption_error(e):
            log("Encryption failed — likely a stale macOS bond; self-healing")
            unpair_macos()
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    last_poll = 0.0
    used_successfully = False
    last_payload: dict | None = None   # most recent API payload (sans cc fields)
    pushed_cc: dict | None = None      # cc fields in the last successful push
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            do_poll = session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL
            if do_poll:
                session.refresh_requested.clear()
                fresh = await poll_active_payload()
                if fresh is None:
                    log("No usable config dir this cycle")
                else:
                    last_payload = fresh
                    last_poll = time.time()
                    pushed_cc = None   # force this fresh payload to be pushed

            # Claude Code status is cheap to read, so check it every tick and
            # push the moment it changes — no extra API calls. On a push failure
            # pushed_cc stays stale, so the next tick retries automatically.
            cc = read_cc_status()
            if last_payload is not None and cc != pushed_cc:
                payload = dict(last_payload)
                payload.update(cc)
                if await session.write_payload(payload):
                    pushed_cc = cc
                    used_successfully = True

            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


async def main() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    backoff = 1
    skip_addr: str | None = None  # macOS: a peripheral to skip for one cycle
    while not stop_event.is_set():
        # Apply any pending skip exactly once, then clear it so the next
        # cycle re-tries retrieveConnected (the device may have recovered).
        target = await discover_target(skip_addr=skip_addr)
        skip_addr = None
        if not target:
            log(f"Device not found, retrying in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
            continue

        addr = target if isinstance(target, str) else target.address
        ok = await connect_and_run(target, stop_event)
        if not ok:
            if sys.platform == "darwin":
                # No string cache to drop; instead skip this stale handle on
                # the next retrieveConnected so the scan fallback is reachable.
                skip_addr = addr
            else:
                log("Invalidating cached address")
                SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
