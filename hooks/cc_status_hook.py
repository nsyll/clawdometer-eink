#!/usr/bin/env python3
"""Clawdometer CC status hook.

Registered on Claude Code's UserPromptSubmit / PreToolUse / Notification / Stop
hooks. On each event it writes ~/.clawd/cc_status.json:

    {"state": "working" | "needs-you" | "question" | "done",
     "message": "<short>", "ts": <epoch>}

The Clawdometer Mac daemon reads that file every ~5s and rides the state through
to the e-paper device over BLE (fields cc/ccm/cct), which draws a status banner.

"question" = Claude ended its turn by asking the user something: either the
AskUserQuestion tool fired, or the final reply text ends with a question mark
(read from the session transcript the Stop hook points at).

Stdlib only; always exits 0 so it can never block a tool call or a turn.
"""
import json
import os
import sys
import tempfile
import time
from pathlib import Path

CC_DIR = Path.home() / ".clawd"
CC_FILE = CC_DIR / "cc_status.json"

# The device draws the question in a small ~10px font across 180px — about 34
# chars. Isolate the final question sentence and trim it to fit.
BANNER_MSG_MAX = 34


def shorten_question(q):
    import re
    q = " ".join(q.split())
    sentences = re.findall(r"[^.!?]*\?", q)
    if sentences:
        q = sentences[-1].strip()
    if len(q) > BANNER_MSG_MAX:
        cut = q[: BANNER_MSG_MAX - 4].rsplit(" ", 1)[0].rstrip(",;:- ")
        q = cut + "...?"
    return q


def ends_question(t):
    """True when text ends in a question mark: ASCII, real Greek erotimatiko
    (U+037E), fullwidth (U+FF1F) -- or an ASCII ';' when the surrounding text
    is actually Greek (Greek keyboards type ';' for the question mark)."""
    if t.endswith("?") or t.endswith("\u037e") or t.endswith("\uff1f"):
        return True
    if t.endswith(";") and any("\u0370" <= c <= "\u03ff" for c in t[-80:]):
        return True
    return False


def last_reply_question(transcript_path):
    """If the session's final assistant text ends with a question, return that
    question line (for the banner's context message); else None. Reads only the
    tail of the transcript so big sessions stay cheap."""
    try:
        size = os.path.getsize(transcript_path)
        with open(transcript_path, "rb") as f:
            f.seek(max(0, size - 262_144))
            chunk = f.read().decode("utf-8", "replace")
    except OSError:
        return None
    for line in reversed(chunk.splitlines()):
        try:
            entry = json.loads(line)
        except ValueError:
            continue
        if entry.get("type") != "assistant":
            continue
        content = (entry.get("message") or {}).get("content") or []
        texts = [b.get("text", "") for b in content
                 if isinstance(b, dict) and b.get("type") == "text"]
        if not texts:
            continue  # tool-use-only entry; keep scanning back for real text
        raw = texts[-1].rstrip()
        cands = [raw]
        # a trailing aside must not hide the question: "flash? (plug cable)"
        if raw.endswith(")") and "(" in raw:
            cands.append(raw[: raw.rfind("(")].rstrip())
        for cand in cands:
            cand = cand.rstrip("*_`\"') ")  # markdown / quote trailers
            if ends_question(cand):
                last_line = cand.rsplit("\n", 1)[-1].strip().lstrip("#>-*_` ")
                return shorten_question(last_line)
        return None  # last reply exists and is not a question
    return None


def main() -> int:
    try:
        data = json.load(sys.stdin)
    except Exception:
        data = {}

    event = data.get("hook_event_name", "")
    tool = str(data.get("tool_name", "") or "")
    if event == "PreToolUse" and tool == "AskUserQuestion":
        # Claude is explicitly asking the user to choose something — show the
        # first question's text (shortened) under the banner.
        q = ""
        try:
            qs = (data.get("tool_input") or {}).get("questions") or []
            if qs and isinstance(qs[0], dict):
                q = shorten_question(str(qs[0].get("question", "") or ""))
        except Exception:
            q = ""
        state, message = "question", q
    elif event in ("UserPromptSubmit", "PreToolUse"):
        # "working" — message hints at what (the tool about to run, if any).
        state, message = "working", tool
    elif event == "Notification":
        # The key one: permission asks / idle-waiting. message has the reason.
        state, message = "needs-you", str(data.get("message", "") or "")
    elif event == "Stop":
        # Finished — but if the reply ends with a question, the ball is in the
        # user's court: surface it as "question" instead of "done".
        q = None
        try:
            tp = str(data.get("transcript_path", "") or "")
            if tp:
                q = last_reply_question(tp)
        except Exception:
            q = None  # heuristics must never break the hook
        state, message = ("question", q) if q else ("done", "")
    else:
        return 0  # unknown event — leave the file untouched

    payload = {"state": state, "message": message[:80], "ts": int(time.time())}

    try:
        CC_DIR.mkdir(parents=True, exist_ok=True)
        # Atomic write so the daemon never reads a half-written file.
        fd, tmp = tempfile.mkstemp(dir=str(CC_DIR), prefix=".cc_status.")
        with os.fdopen(fd, "w") as f:
            json.dump(payload, f)
        os.replace(tmp, str(CC_FILE))
    except OSError:
        pass  # best-effort; never fail the hook

    return 0


if __name__ == "__main__":
    sys.exit(main())
