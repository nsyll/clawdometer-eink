#pragma once
// FUNCTIONALITY (clawdometer app): shared state + the usage-meter screen.
#include <Arduino.h>

struct ClawdState {
  int   session   = -1;   // 5h utilization %
  int   sessReset = -1;   // minutes until 5h reset
  int   weekly    = -1;   // 7d utilization %
  int   weekReset = -1;   // minutes until 7d reset
  char  status[16] = "";  // status string from daemon
  char  acct[8]    = "";  // "pro" | "ent"
  long  epoch      = 0;   // unix time from daemon (t)
  int   timeFmt    = 24;  // 12 | 24 (tf)
  char  cc[16]     = "";  // Claude Code state: working|needs-you|done (cc)
  char  ccMsg[48]  = "";  // short CC context message (ccm)
  long  ccTs       = 0;   // ts of the CC status (cct)
  int   batt       = -1;  // battery charge % (-1 = unknown), read on the device
  bool  chimeOn    = true; // needs-you chime enabled (shows a mute icon when off)
  long  daily[30]  = {0}; // last 30 days tokens/1000, oldest..newest
  long  hourly[24] = {0}; // today's tokens/1000 per hour-of-day
  int   histLen    = 0;   // number of valid daily entries (0 = none yet)
  long  sTk[3]     = {0}; // period tokens/1000        [0]=today [1]=7d [2]=30d
  int   sWk[3]     = {0}; // period Claude-working minutes
  int   sYt[3]     = {0}; // period your-turn minutes
  int   sSe[3]     = {0}; // period session count
  int   sTl[3]     = {0}; // period tool-call count
  int   view       = 0;   // 0=meters, 1=today(hourly), 2=7d, 3=30d (BOOT cycles)
  bool  sleepBadge = false; // deep-sleep render: zZ marker (image is last-known state)
  bool  haveData   = false;
  bool  linked     = false; // BLE central connected
};

void renderClawd(const ClawdState& s, bool full);
void renderBatteryEmpty();   // farewell screen drawn just before the LiPo cutoff
