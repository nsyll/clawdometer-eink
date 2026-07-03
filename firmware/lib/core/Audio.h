#pragma once
// CORE: short chimes via the onboard ES8311 codec + I2S.

// Distinct playful jingles — the ear tells events apart without looking.
enum ChimeId {
  CHIME_NEEDS_YOU,   // knock-knock: two insistent same-pitch beeps
  CHIME_QUESTION,    // rising "hm-hmm?" — question intonation
  CHIME_DONE,        // ta-daa! rising major arpeggio (work finished)
  CHIME_THROTTLED,   // low falling "uh-oh" (rate limit hit, work stopped)
  CHIME_RECOVERED,   // quick bright up-flick (limit lifted, all clear)
  CHIME_LOW_BATT,    // slow drooping notes, quieter (charge me)
};

void audioInit();           // power codec + I2S (call after the I2C bus is up)
void chimePlay(ChimeId id); // play one jingle (amp toggles around it)
void chime(bool workDone);  // legacy 2-tone API -> DONE / NEEDS_YOU jingles
