// ============================================================================
//  Clawdometer app for the ESP32-S3 e-Paper board.
//  Pairs over BLE with Clawdometer's (unmodified) Mac daemon and shows Claude
//  Code session/weekly rate-limit utilization. Reuses the shared Core + Ble.
// ============================================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "driver/gpio.h"

#include "board_pins.h"
#include "BoardPower.h"
#include "Display.h"
#include "Buttons.h"
#include "Sensors.h"
#include "Audio.h"
#include "Ble.h"
#include "screen.h"

// Clawdometer GATT contract (must match the daemon)
#define CLAWD_NAME "Clawdometer"
#define SVC_UUID   "4c41555a-4465-7669-6365-000000000001"
#define RX_UUID    "4c41555a-4465-7669-6365-000000000002"  // daemon writes JSON here
#define REQ_UUID   "4c41555a-4465-7669-6365-000000000004"  // device -> daemon refresh request

static Ble        ble;
static Button     btnBoot, btnPwr;
static ClawdState g_st;
static Preferences g_prefs;
static volatile bool g_dirty = false;
// Chime requests: one bit per ChimeId; the loop plays the most important one.
static volatile uint8_t g_chimeMask = 0;
static inline void reqChime(ChimeId id) { g_chimeMask |= (uint8_t)(1 << id); }
static bool g_chimeOn = true;               // chime enabled (persisted in NVS)
static unsigned long g_lastRenderMs = 0;
static const unsigned long HEARTBEAT_MS = 5UL * 60 * 1000;  // 5-min idle redraw

// --- SLEEP mode: deep-sleep when nobody is around. The e-paper keeps the last
// image at zero power, so the user loses nothing but instant updates. Any
// button wakes it; a 30-min timer wake sniffs for the daemon (90s covers the
// daemon's 60s-capped retry backoff + 8s scan).
static const unsigned long UNLINKED_SLEEP_MS = 5UL * 60 * 1000;   // no Mac found
static const unsigned long SILENT_SLEEP_MS   = 20UL * 60 * 1000;  // linked but daemon mute
static const unsigned long SNIFF_WINDOW_MS   = 90UL * 1000;       // timer-wake look-around
static const uint64_t      SLEEP_TIMER_US    = 30ULL * 60 * 1000000;
static RTC_DATA_ATTR bool g_sleptBadge = false;  // zZ already on the persisted image
static unsigned long g_lastLinkMs = 0;   // last time the BLE link was up (or a button woke us)
static unsigned long g_lastDataMs = 0;   // last daemon push
static bool g_sniff = false;             // in a timer-wake sniff window
static unsigned long g_sniffDeadline = 0;

static void goToSleep() {
  Serial.println("[Clawd] deep sleep (button or 30-min timer wakes)");
  if (!g_sleptBadge) {
    // Re-render once with the zZ marker; clear the realtime CC/status banners —
    // showing WORKING while asleep would be a lie. Meters/stats stay (last known).
    ClawdState s = g_st;
    s.sleepBadge = true;
    s.cc[0] = 0;
    s.status[0] = 0;
    s.batt = batteryPercent();
    renderClawd(s, true);                // ends in display.hibernate()
    g_sleptBadge = true;
  }
  epdPower(false);                       // panel rail off (image persists unpowered)
  digitalWrite(AUD_PA_CTRL, LOW);        // amp off
  digitalWrite(AUD_PA_EN, HIGH);         // codec off (active-low)
  // Hold pad states through deep sleep. VBAT_HOLD is the critical one: if it
  // drops, the battery latch releases and the board powers itself off.
  gpio_hold_en((gpio_num_t)VBAT_HOLD);
  gpio_hold_en((gpio_num_t)EPD_PWR);
  gpio_hold_en((gpio_num_t)AUD_PA_EN);
  gpio_hold_en((gpio_num_t)AUD_PA_CTRL);
  gpio_deep_sleep_hold_en();
  // Buttons are active-low; keep their pull-ups alive in the RTC domain.
  rtc_gpio_pullup_en((gpio_num_t)BTN_BOOT);  rtc_gpio_pulldown_dis((gpio_num_t)BTN_BOOT);
  rtc_gpio_pullup_en((gpio_num_t)BTN_PWR);   rtc_gpio_pulldown_dis((gpio_num_t)BTN_PWR);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  esp_sleep_enable_ext1_wakeup((1ULL << BTN_BOOT) | (1ULL << BTN_PWR),
                               ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_enable_timer_wakeup(SLEEP_TIMER_US);
  Serial.flush();
  esp_deep_sleep_start();
}

// Parse the daemon's JSON push into g_st. Only flag a redraw when something the
// user would notice changed: the CC *state*, the rate-limit status, or either
// utilization %. We deliberately ignore the reset countdown (sr/wr), the
// per-tool reason (ccm) and the status timestamp (cct) — they churn constantly
// and would make the e-paper flash. The 5-min heartbeat picks up any drift.
static void onBleData(const std::string& json) {
  JsonDocument d;
  if (deserializeJson(d, json) != DeserializationError::Ok) {
    Serial.println("[Clawd] JSON parse error");
    return;
  }
  int newS = d["s"] | -1;
  int newW = d["w"] | -1;
  const char* newSt = d["st"] | "";
  const char* newCc = d["cc"] | "";

  bool changed = !g_st.haveData
              || newS != g_st.session
              || newW != g_st.weekly
              || strcmp(newSt, g_st.status) != 0
              || strcmp(newCc, g_st.cc)     != 0;

  // Distinct jingles on transitions (never on repeats, so reconnects stay quiet):
  // knock-knock = needs you, "hm-hmm?" = question, ta-daa = finished,
  // "uh-oh" = throttled, up-flick = limit lifted.
  if (!strcmp(newCc, "needs-you") && strcmp(g_st.cc, "needs-you") != 0)
    reqChime(CHIME_NEEDS_YOU);
  else if (!strcmp(newCc, "question") && strcmp(g_st.cc, "question") != 0)
    reqChime(CHIME_QUESTION);
  if (!strcmp(newCc, "done") && !strcmp(g_st.cc, "working"))
    reqChime(CHIME_DONE);

  bool wasThr = !strcmp(g_st.status, "rejected") || !strcmp(g_st.status, "limited");
  bool nowThr = !strcmp(newSt, "rejected")       || !strcmp(newSt, "limited");
  if (nowThr && !wasThr && g_st.haveData) reqChime(CHIME_THROTTLED);
  if (!nowThr && wasThr)                  reqChime(CHIME_RECOVERED);

  g_st.session   = newS;
  g_st.sessReset = d["sr"] | -1;
  g_st.weekly    = newW;
  g_st.weekReset = d["wr"] | -1;
  g_st.epoch     = d["t"]  | 0L;
  g_st.timeFmt   = d["tf"] | 24;
  snprintf(g_st.status, sizeof(g_st.status), "%s", newSt);
  snprintf(g_st.acct,   sizeof(g_st.acct),   "%s", (const char*)(d["acct"] | ""));
  snprintf(g_st.cc,     sizeof(g_st.cc),     "%s", newCc);
  snprintf(g_st.ccMsg,  sizeof(g_st.ccMsg),  "%s", (const char*)(d["ccm"] | ""));
  g_st.ccTs = d["cct"] | 0L;

  // Usage stats (don't trigger a redraw; only shown on the stats views via BOOT)
  JsonArrayConst hh = d["hh"].as<JsonArrayConst>();
  if (!hh.isNull()) { int n = 0; for (JsonVariantConst v : hh) if (n < 24) g_st.hourly[n++] = v.as<long>(); }
  JsonArrayConst h = d["h"].as<JsonArrayConst>();
  if (!h.isNull()) { int n = 0; for (JsonVariantConst v : h) if (n < 30) g_st.daily[n++] = v.as<long>(); g_st.histLen = n; }
  const char* keys[3] = {"d", "d7", "d30"};
  for (int i = 0; i < 3; i++) {
    JsonObjectConst o = d[keys[i]].as<JsonObjectConst>();
    if (o.isNull()) continue;
    g_st.sTk[i] = o["tk"] | 0L;
    g_st.sWk[i] = o["wk"] | 0;
    g_st.sYt[i] = o["yt"] | 0;
    g_st.sSe[i] = o["se"] | 0;
    g_st.sTl[i] = o["tl"] | 0;
  }

  g_st.haveData = true;
  g_lastDataMs = millis();
  g_sniff = false;               // daemon is alive -> stay awake, normal rules
  if (changed) g_dirty = true;
  Serial.printf("[Clawd] s=%d w=%d st=%s cc=%s changed=%d\n",
                newS, newW, newSt, newCc, changed);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n== Clawdometer (e-Paper) ==");

  // Deep-sleep wake? Release the pad holds set by goToSleep() so GPIO writes
  // take effect again, and remember how we woke up.
  esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
  bool fromSleep = (wake == ESP_SLEEP_WAKEUP_EXT1 || wake == ESP_SLEEP_WAKEUP_TIMER);
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)VBAT_HOLD);
  gpio_hold_dis((gpio_num_t)EPD_PWR);
  gpio_hold_dis((gpio_num_t)AUD_PA_EN);
  gpio_hold_dis((gpio_num_t)AUD_PA_CTRL);

  boardPowerInit();          // VBAT latch + e-paper panel power
  cpuLowPower(80);
  btnBoot.begin(BTN_BOOT);
  btnPwr.begin(BTN_PWR);

  sensorsBegin();            // brings up the I2C bus (needed before audioInit)
  audioInit();               // ES8311 codec + I2S (amp stays off until a chime)

  g_prefs.begin("clawd", false);
  g_chimeOn = g_prefs.getBool("chime", true);   // default on

  displayBegin();
  g_st.linked = false;
  g_st.chimeOn = g_chimeOn;
  g_st.batt = batteryPercent();
  if (!fromSleep) {
    renderClawd(g_st, true);   // "waiting for Mac..." (cold boot only)
    g_sleptBadge = false;
  } else {
    // Woken from deep sleep: the panel still shows the last state (with zZ).
    // Don't touch it — the next daemon push redraws and clears the badge.
    Serial.printf("[Clawd] woke: %s\n",
                  wake == ESP_SLEEP_WAKEUP_TIMER ? "timer (sniff)" : "button");
  }
  g_lastRenderMs = millis();
  g_lastLinkMs = millis();
  g_lastDataMs = millis();
  if (wake == ESP_SLEEP_WAKEUP_TIMER) {
    g_sniff = true;            // short look-around; back to sleep if no daemon
    g_sniffDeadline = millis() + SNIFF_WINDOW_MS;
  }

  ble.onReceive(onBleData);
  ble.begin(CLAWD_NAME, SVC_UUID, RX_UUID, REQ_UUID);
}

void loop() {
  unsigned long now = millis();

  // track BLE link state for the UI. After a sleep-wake the panel still shows
  // the (useful) last image — don't let a dataless link flip wipe it.
  bool linkedNow = ble.connected();
  if (linkedNow) g_lastLinkMs = now;
  if (linkedNow != g_st.linked) {
    g_st.linked = linkedNow;
    if (g_st.haveData || !g_sleptBadge) g_dirty = true;
  }

  // SLEEP decisions (before any render): nobody around -> deep sleep.
  if (g_sniff && now > g_sniffDeadline) goToSleep();
  if (!g_sniff) {
    if (!linkedNow && now - g_lastLinkMs > UNLINKED_SLEEP_MS) goToSleep();
    if (linkedNow  && now - g_lastDataMs > SILENT_SLEEP_MS)   goToSleep();
  }

  // BOOT: cycle meters -> today(hourly) -> 7 days -> 30 days -> meters.
  if (btnBoot.poll() == BTN_SHORT) {
    g_lastLinkMs = now;              // user present: reset the sleep timer
    g_sniff = false;
    if (g_st.haveData) {             // without data there is nothing to cycle
      g_st.view = (g_st.view + 1) % 4;
      g_dirty = true;
      Serial.printf("[Clawd] view %d\n", g_st.view);
    }
  }
  // PWR: short press toggles the needs-you chime (persisted); hold re-advertises.
  ButtonEvent pe = btnPwr.poll();
  if (pe == BTN_LONG) {
    Serial.println("[Clawd] re-advertise");
    ble.readvertise();
  } else if (pe == BTN_SHORT) {
    g_lastLinkMs = now;              // user present: reset the sleep timer
    g_sniff = false;
    g_chimeOn = !g_chimeOn;
    g_prefs.putBool("chime", g_chimeOn);
    g_st.chimeOn = g_chimeOn;
    if (g_st.haveData) g_dirty = true;   // redraw to show/hide the mute icon
    Serial.printf("[Clawd] chime %s\n", g_chimeOn ? "on" : "off");
  }

  // LiPo protection: on battery and truly empty (<3% twice in a row, checked
  // once a minute), draw the farewell screen and release the power latch —
  // a controlled power-off instead of a brown-out at a random moment.
  static unsigned long lastBattCheckMs = 0;
  static int emptyCount = 0;
  if (millis() - lastBattCheckMs >= 60000UL) {
    lastBattCheckMs = millis();
    int b = batteryPercent();
    if (b >= 0 && b < 3 && !usbPowered()) emptyCount++;
    else emptyCount = 0;
    if (emptyCount >= 2) {
      Serial.println("[Clawd] battery empty -> latch off");
      renderBatteryEmpty();
      epdPower(false);
      digitalWrite(VBAT_HOLD, LOW);   // release the latch: hard power-off
      while (true) delay(1000);       // unreachable on battery power
    }
  }

  // low-battery jingle: once per discharge (re-arms after charging past 15%)
  static bool lowBattChimed = false;
  if (g_st.batt >= 0 && g_st.batt < 10 && !lowBattChimed) {
    reqChime(CHIME_LOW_BATT);
    lowBattChimed = true;
  } else if (g_st.batt > 15) {
    lowBattChimed = false;
  }

  // play the single most important pending jingle (respects the PWR mute)
  if (g_chimeMask) {
    uint8_t m = g_chimeMask;
    g_chimeMask = 0;
    if (g_chimeOn) {
      static const ChimeId prio[] = {CHIME_NEEDS_YOU, CHIME_QUESTION, CHIME_THROTTLED,
                                     CHIME_RECOVERED, CHIME_DONE, CHIME_LOW_BATT};
      for (ChimeId c : prio) {
        if (m & (1 << c)) { chimePlay(c); break; }
      }
    }
  }

  // 5-min heartbeat so battery / reset-time drift / clock stay reasonably fresh
  // even when no field we redraw on has changed. Pointless without data (and it
  // would wipe the image preserved across a sleep-wake).
  if (g_st.haveData && millis() - g_lastRenderMs >= HEARTBEAT_MS) g_dirty = true;

  if (g_dirty) {
    g_dirty = false;
    g_st.batt = batteryPercent();   // refresh the reading whenever we redraw anyway
    renderClawd(g_st, true);   // full refresh: crisp, infrequent (only on real change)
    g_lastRenderMs = millis();
    g_sleptBadge = false;      // live image on screen -> next sleep re-badges it
  }

  delay(50);
}
