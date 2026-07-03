// FUNCTIONALITY (clawdometer app): render Claude session/weekly usage on e-paper.
#include <Arduino.h>
#include <string.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>

#include "Display.h"
#include "screen.h"
#include "reset_font.h"   // small FreeSans-style font for the reset time

#define CLAWD_ROTATION 1   // button on top, USB on the right (matches the user's setup)

// Copy s into out (size n) trimmed so it renders within maxW px in the CURRENT font.
static void fitText(const char* s, int maxW, char* out, size_t n) {
  snprintf(out, n, "%s", s);
  int16_t x, y; uint16_t w, h;
  while (out[0]) {
    display.getTextBounds(out, 0, 0, &x, &y, &w, &h);
    if ((int)w <= maxW) break;
    out[strlen(out) - 1] = '\0';   // drop the last char and re-measure
  }
}

// Map the Claude Code state to a banner label; *highlight => inverted (attention) bar.
static const char* ccBannerLabel(const char* cc, bool* highlight) {
  *highlight = false;
  if (!strcmp(cc, "needs-you")) { *highlight = true; return "NEEDS YOU"; }
  if (!strcmp(cc, "question"))  { *highlight = true; return "QUESTION"; }
  if (!strcmp(cc, "working"))   return "WORKING";
  if (!strcmp(cc, "done"))      return "DONE";
  return cc;   // fallback: show whatever the daemon sent
}

// Map a rate-limit status (the daemon's "st") to a banner label. Returns NULL
// when nothing should be shown (normal operation). *urgent => inverted bar.
// Known Anthropic values are "allowed" / "allowed_warning" / "rejected"; the
// firmware also saw "limited". Anything unmapped is shown raw so we never hide
// a real state we haven't seen yet.
static const char* statusBannerLabel(const char* st, bool* urgent) {
  *urgent = false;
  if (!st[0] || !strcmp(st, "allowed") || !strcmp(st, "unknown")) return nullptr;
  if (!strcmp(st, "allowed_warning"))                return "LIMIT NEAR";
  if (!strcmp(st, "rejected") || !strcmp(st, "limited")) { *urgent = true; return "THROTTLED"; }
  *urgent = true; return st;   // unknown non-normal status: surface it raw
}

// Shared alert box in the zone just below the header rule.
// urgent => filled/inverted (loud); otherwise outlined.
static void drawBannerBox(const char* label, bool urgent) {
  const int bx = 10, by = 40, bw = 200 - 2 * 10, bh = 26;
  if (urgent) {
    display.fillRect(bx, by, bw, bh, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
  } else {
    display.drawRect(bx, by, bw, bh, GxEPD_BLACK);
    display.setTextColor(GxEPD_BLACK);
  }
  display.setFont(&FreeSansBold12pt7b);
  int16_t tx, ty; uint16_t tw, th;
  char ft[20]; fitText(label, bw - 8, ft, sizeof(ft));
  display.getTextBounds(ft, 0, 0, &tx, &ty, &tw, &th);
  int cx = bx + (bw - (int)tw) / 2;
  if (cx < bx + 4) cx = bx + 4;
  int cy = by + (bh - (int)th) / 2 - ty;   // vertically centered (equal top/bottom padding)
  display.setCursor(cx, cy);
  display.print(ft);
  display.setTextColor(GxEPD_BLACK);   // restore for the rest of the screen
}

// CC status banner + its reason (e.g. the permission Claude is waiting on).
static void drawCcBanner(const ClawdState& s) {
  bool hi; const char* lbl = ccBannerLabel(s.cc, &hi);
  drawBannerBox(lbl, hi);
  if (s.ccMsg[0]) {
    display.setFont(&ResetFont);   // small (~10px) so the reason stays discreet
    int16_t tx, ty; uint16_t tw, th;
    char ft[48]; fitText(s.ccMsg, 200 - 2 * 10, ft, sizeof(ft));
    display.getTextBounds(ft, 0, 0, &tx, &ty, &tw, &th);
    int rx = 10 + ((200 - 2 * 10) - (int)tw) / 2;
    if (rx < 10) rx = 10;
    display.setCursor(rx, 40 + 26 + 16);   // under the banner
    display.print(ft);
    display.setFont(&FreeSans9pt7b);
  }
}

// minutes -> compact "Xd" / "XhYY" / "Xm"
static void fmtReset(int mins, char* out, size_t n) {
  if (mins < 0)            snprintf(out, n, "--");
  else if (mins >= 1440)   snprintf(out, n, "%dd", mins / 1440);
  else if (mins >= 60)     snprintf(out, n, "%dh%02d", mins / 60, mins % 60);
  else                     snprintf(out, n, "%dm", mins);
}

// labelled progress bar with a right-aligned percentage (on the label line)
// and a right-aligned reset hint ("rst Xh", level with the bar) — both numbers
// share one tidy column down the right edge.
static void drawMeter(int y, const char* label, int pct, int resetMin) {
  const int PAD = 10;
  const int RIGHT = 200 - PAD;          // right margin the numbers align to
  int16_t tx, ty; uint16_t tw, th;
  display.setFont(&FreeSans9pt7b);

  // top line: label (left) + percentage (right)
  display.setCursor(PAD, y);
  display.print(label);
  char pb[8];
  snprintf(pb, sizeof(pb), pct >= 0 ? "%d%%" : "--", pct);
  display.getTextBounds(pb, 0, 0, &tx, &ty, &tw, &th);
  display.setCursor(RIGHT - tw, y);
  display.print(pb);

  // full-width progress bar on the next line
  const int barW = 120, barH = 12, barX = PAD, barY = y + 8;
  display.drawRect(barX, barY, barW, barH, GxEPD_BLACK);
  if (pct >= 0) {
    int fill = (pct > 100 ? 100 : pct) * (barW - 4) / 100;
    if (fill > 0) display.fillRect(barX + 2, barY + 2, fill, barH - 4, GxEPD_BLACK);
  }

  // reset time, smaller FreeSans-style font, right-aligned beneath the % — its
  // bottom lined up with the bottom of the bar (baseline = bar bottom)
  char tmp[10]; fmtReset(resetMin, tmp, sizeof(tmp));
  display.setFont(&ResetFont);
  display.getTextBounds(tmp, 0, 0, &tx, &ty, &tw, &th);
  display.setCursor(RIGHT - tw, barY + barH);
  display.print(tmp);
  display.setFont(&FreeSans9pt7b);   // restore default for anything after
}

// Small speaker-with-slash, drawn in the header when the needs-you chime is off.
static void drawMuteIcon(int x, int y) {
  display.fillRect(x, y + 4, 3, 5, GxEPD_BLACK);                                 // driver box
  display.fillTriangle(x + 3, y + 6, x + 9, y + 1, x + 9, y + 11, GxEPD_BLACK);  // cone
  display.drawLine(x - 1, y + 12, x + 11, y, GxEPD_BLACK);                       // mute slash
  display.drawLine(x,     y + 12, x + 12, y, GxEPD_BLACK);                       // (2px thick)
}

// token count given in THOUSANDS: 692 -> "692K", 7179 -> "7.1M", 0 -> "0"
static void fmtNum(long k, char* out, size_t n) {
  if (k >= 1000)  snprintf(out, n, "%ld.%ldM", k / 1000, (k % 1000) / 100);
  else if (k > 0) snprintf(out, n, "%ldK", k);
  else            snprintf(out, n, "0");
}

// Stats page for one period (BOOT cycles today -> 7d -> 30d). Shows the period's
// usage numbers + a bar chart (today=per hour, 7d/30d=per day). Local logs, backfilled.
static void drawStats(const ClawdState& s) {
  const int PAD = 10, RIGHT = 200 - PAD;
  int16_t bx, by; uint16_t bw, bh;
  int p = s.view - 1; if (p < 0) p = 0; if (p > 2) p = 2;   // 0=today 1=7d 2=30d
  const char* titles[3] = {"TODAY", "7 DAYS", "30 DAYS"};
  display.setTextColor(GxEPD_BLACK);

  display.setFont(&FreeSansBold12pt7b);
  display.setCursor(PAD, 22);
  display.print(titles[p]);
  display.drawFastHLine(PAD, 30, 200 - 2 * PAD, GxEPD_BLACK);

  display.setFont(&FreeSans9pt7b);
  const char* labels[5] = {"tokens", "Claude working", "your turn", "sessions", "tool calls"};
  char vals[5][16];
  fmtNum(s.sTk[p], vals[0], sizeof(vals[0]));
  snprintf(vals[1], sizeof(vals[1]), "%dm", s.sWk[p]);
  snprintf(vals[2], sizeof(vals[2]), "%dm", s.sYt[p]);
  snprintf(vals[3], sizeof(vals[3]), "%d", s.sSe[p]);
  snprintf(vals[4], sizeof(vals[4]), "%d", s.sTl[p]);
  for (int i = 0; i < 5; i++) {
    int y = 48 + i * 19;
    display.setCursor(PAD, y);
    display.print(labels[i]);
    display.getTextBounds(vals[i], 0, 0, &bx, &by, &bw, &bh);
    display.setCursor(RIGHT - bw, y);
    display.print(vals[i]);
  }
  display.drawFastHLine(PAD, 142, 200 - 2 * PAD, GxEPD_BLACK);

  // bar chart: today=hourly(24), 7d=last 7 daily, 30d=all 30 daily
  const long* bars; int nb; const char* dlabel;
  if      (p == 0) { bars = s.hourly;      nb = 24; dlabel = "per hour"; }
  else if (p == 1) { bars = &s.daily[23];  nb = 7;  dlabel = "per day (7d)"; }
  else             { bars = s.daily;       nb = 30; dlabel = "per day (30d)"; }
  display.setCursor(PAD, 160);
  display.print(dlabel);

  const int base = 190, top = 168, sparkH = base - top;   // 22px tall
  long mx = 1;
  for (int i = 0; i < nb; i++) if (bars[i] > mx) mx = bars[i];
  int slot = (200 - 2 * PAD) / nb; if (slot < 1) slot = 1;
  int barW = slot - 1; if (barW < 1) barW = 1;
  for (int i = 0; i < nb; i++) {
    long v = bars[i]; if (v < 0) v = 0;
    int hh = (int)(v * sparkH / mx);
    int x = PAD + i * slot;
    if (hh > 0) display.fillRect(x, base - hh, barW, hh, GxEPD_BLACK);
    else        display.drawPixel(x, base, GxEPD_BLACK);
  }
  display.drawFastHLine(PAD, base, 200 - 2 * PAD, GxEPD_BLACK);
}

// Last screen before the battery latch is released: a big drained battery so
// the "why is it off?" question answers itself from across the room. The image
// persists on the unpowered panel until the next charge.
void renderBatteryEmpty() {
  display.setRotation(CLAWD_ROTATION);
  display.setTextColor(GxEPD_BLACK);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    const int bx = 62, by = 48, bw = 70, bh = 34;   // big battery outline, centered
    display.drawRect(bx, by, bw, bh, GxEPD_BLACK);
    display.drawRect(bx + 1, by + 1, bw - 2, bh - 2, GxEPD_BLACK);
    display.fillRect(bx + bw + 1, by + 10, 5, 14, GxEPD_BLACK);   // + nub
    display.fillRect(bx + 5, by + 6, 6, bh - 12, GxEPD_BLACK);    // last sliver

    int16_t tx, ty; uint16_t tw, th;
    display.setFont(&FreeSansBold12pt7b);
    display.getTextBounds("BATTERY EMPTY", 0, 0, &tx, &ty, &tw, &th);
    display.setCursor((200 - (int)tw) / 2, 128);
    display.print("BATTERY EMPTY");
    display.setFont(&FreeSans9pt7b);
    display.getTextBounds("charge me!", 0, 0, &tx, &ty, &tw, &th);
    display.setCursor((200 - (int)tw) / 2, 154);
    display.print("charge me!");
  } while (display.nextPage());
  display.hibernate();
}

// Small "zZ" marker: the device is deep-sleeping and this image is the
// last-known state (any button wakes it). Stats views get a bare corner
// marker; the meters view has room for a word in the empty banner zone.
static void drawSleepBadge(bool cornerOnly) {
  display.setFont(&FreeSans9pt7b);
  if (cornerOnly) {
    int16_t bx, by; uint16_t bw, bh;
    display.getTextBounds("zZ", 0, 0, &bx, &by, &bw, &bh);
    display.setCursor(200 - 10 - bw, 22);
    display.print("zZ");
  } else {
    display.setCursor(10, 66);
    display.print("zZ  sleeping");
  }
}

void renderClawd(const ClawdState& s, bool full) {
  display.setRotation(CLAWD_ROTATION);
  display.setTextColor(GxEPD_BLACK);
  full ? display.setFullWindow() : display.setPartialWindow(0, 0, 200, 200);

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    if (s.view != 0 && s.haveData) {
      drawStats(s);
      if (s.sleepBadge) drawSleepBadge(true);
      continue;
    }

    int16_t bx, by; uint16_t bw, bh;
    const int PAD = 10;

    // header: title (left) + battery icon & % (right)
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(PAD, 22);
    display.print("CLAUDE");
    if (s.batt >= 0) {
      display.setFont(&FreeSans9pt7b);
      char bb[6]; snprintf(bb, sizeof(bb), "%d%%", s.batt);
      display.getTextBounds(bb, 0, 0, &bx, &by, &bw, &bh);
      const int icoW = 20, icoX = 200 - PAD - icoW;   // 18 body + 2 nub
      const int claudeMid = 14;                       // vertical middle of CLAUDE (cap 18 @ baseline 22)
      int baseY = claudeMid - by - (int)bh / 2;        // center the % on CLAUDE's midline
      drawBatteryIcon(icoX, claudeMid - 9 / 2, s.batt);// center the 9px battery there too
      display.setCursor(icoX - 4 - bw, baseY);
      display.print(bb);
    }
    if (!s.chimeOn) drawMuteIcon(112, 9);   // chime muted -> speaker-slash in the header
    display.drawFastHLine(PAD, 30, 200 - 2 * PAD, GxEPD_BLACK);

    if (!s.linked) {
      display.setFont(&FreeSans9pt7b);
      display.setCursor(PAD, 110);
      display.print("waiting for Mac...");
      display.setCursor(PAD, 132);
      display.print("(run the daemon)");
    } else if (!s.haveData) {
      display.setFont(&FreeSans9pt7b);
      display.setCursor(PAD, 110);
      display.print("linked - no data yet");
    } else {
      // Alert zone (below the header). The meters never move; the banner lives
      // in the space above them, empty when all is calm.
      // Alert banner: the CC state wins; otherwise a notable rate-limit status.
      // "done" is the resting/your-turn state — show no banner for it. Only
      // WORKING and NEEDS YOU draw a CC banner.
      bool surgent; const char* slbl;
      if (s.cc[0] && strcmp(s.cc, "done") != 0) {
        drawCcBanner(s);                                  // WORKING / NEEDS YOU + reason
      } else if ((slbl = statusBannerLabel(s.status, &surgent)) != nullptr) {
        drawBannerBox(slbl, surgent);                     // throttle/limit warning
      }

      // usage meters — fixed near the bottom, independent of the banner above.
      // Weekly stays bottom-aligned; Session sits higher for a roomier gap.
      drawMeter(108, "Session 5h", s.session, s.sessReset);
      drawMeter(160, "Weekly 7d",  s.weekly,  s.weekReset);
    }
    if (s.sleepBadge) drawSleepBadge(false);
  } while (display.nextPage());
  display.hibernate();   // SSD1681 deep-sleep between refreshes (~1uA); GxEPD2 re-wakes it via RST
}
