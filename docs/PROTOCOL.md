# Protocol reference

Three tiny interfaces glue the system together.

## 1. GATT contract (device ⇄ daemon)

| | UUID |
|---|---|
| Device name | `Clawdometer` |
| Service | `4c41555a-4465-7669-6365-000000000001` |
| RX characteristic (daemon **writes** JSON here) | `4c41555a-4465-7669-6365-000000000002` |
| REQ characteristic (device **notifies** to request a refresh) | `4c41555a-4465-7669-6365-000000000004` |

The device requests MTU 512 — the payload (~450 B) must fit one write. Writes are `write-without-response`.

## 2. BLE JSON payload (daemon → device, every ~60 s)

```jsonc
{
  "s": 24,          // session (5h) utilization %
  "sr": 159,        // minutes until 5h reset
  "w": 4,           // weekly (7d) utilization %
  "wr": 3899,       // minutes until 7d reset
  "st": "allowed",  // rate-limit status: allowed | allowed_warning | rejected | limited
  "acct": "pro",    // pro | ent
  "ok": true,

  // Claude Code status (omitted when no fresh hook data)
  "cc": "question",           // working | needs-you | question | done
  "ccm": "Ready to flash?",   // short context message (≤80 chars)
  "cct": 1783076670,          // epoch of the status

  // usage stats, computed from local Claude Code logs
  "hh": [0,0, ...],           // 24 ints — today's tokens per hour, in thousands
  "h":  [0,221, ...],         // 30 ints — daily tokens, oldest..newest, in thousands
  "d":   {"tk":200,  "wk":25,  "yt":5,   "se":3,  "tl":58},    // today
  "d7":  {"tk":7243, "wk":557, "yt":653, "se":13, "tl":1699},  // last 7 days
  "d30": {"tk":13648,"wk":881, "yt":990, "se":22, "tl":2627}   // last 30 days
  // tk = tokens (thousands), wk = Claude-working minutes,
  // yt = your-turn minutes, se = sessions, tl = tool calls
}
```

Device-side redraw policy: only `s`, `w`, `st`, and the `cc` **state** trigger a refresh; everything else is stored silently and shown at the next redraw (e-paper flashes on refresh, so churn is deliberately ignored).

## 3. CC status file (hook → daemon)

`~/.clawd/cc_status.json`, written atomically on every hook event:

```json
{ "state": "working" | "needs-you" | "question" | "done",
  "message": "<short context, ≤80 chars>",
  "ts": 1783076670 }
```

The daemon forwards it as `cc`/`ccm`/`cct` and drops it when `ts` is older than 30 minutes (a crashed session must not pin a stale banner).

### State semantics

| state | meaning | device banner | jingle |
|---|---|---|---|
| `working` | Claude is running (prompt submitted / tool executing) | `WORKING` + tool name | — |
| `needs-you` | permission prompt or idle-waiting | `NEEDS YOU` (inverted) + reason | knock-knock |
| `question` | Claude ended its turn asking the user something | `QUESTION` (inverted) + the question | rising "hm-hmm?" |
| `done` | turn finished, nothing pending | *(no banner)* | ta-daa (only on working→done) |

Additional device-local events: `st` → `rejected`/`limited` plays "uh-oh" and back to `allowed` plays the recovery up-flick; battery <10% plays the low-battery droop once per discharge.
