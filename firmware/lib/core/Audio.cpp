// ============================================================================
//  CORE: Audio chime via the onboard ES8311 codec.
//  Legacy I2S driver (Arduino-ESP32 2.0.x) with MCLK. The ES8311 will not ACK
//  on I2C until MCLK is running, so I2S is started BEFORE the codec I2C init.
// ============================================================================
#include <Arduino.h>
#include <math.h>
#include "driver/i2s.h"
#include "driver/i2c.h"
#include "es8311.h"
#include "board_pins.h"
#include "Audio.h"

#define I2S_PORT    I2S_NUM_0
#define SAMPLE_RATE 24000
#define MCLK_MULT   256

static bool s_ready = false;

void audioInit() {
  pinMode(AUD_PA_EN, OUTPUT);   digitalWrite(AUD_PA_EN, LOW);     // codec power ON (active-low)
  pinMode(AUD_PA_CTRL, OUTPUT); digitalWrite(AUD_PA_CTRL, HIGH);  // amp enable
  delay(100);

  // I2S master TX with MCLK -- started first so MCLK is live for the codec
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate          = SAMPLE_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = 0;
  cfg.dma_buf_count        = 6;
  cfg.dma_buf_len          = 256;
  cfg.use_apll             = true;
  cfg.tx_desc_auto_clear   = true;
  cfg.fixed_mclk           = SAMPLE_RATE * MCLK_MULT;
  cfg.mclk_multiple        = I2S_MCLK_MULTIPLE_256;
  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) {
    Serial.println("[Audio] i2s install failed"); return;
  }
  i2s_pin_config_t pins = {};
  pins.mck_io_num   = AUD_MCLK;
  pins.bck_io_num   = AUD_BCLK;
  pins.ws_io_num    = AUD_WS;
  pins.data_out_num = AUD_DOUT;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
  delay(50);

  es8311_handle_t h = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  if (!h) { Serial.println("[Audio] es8311 create failed"); return; }
  es8311_clock_config_t clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = SAMPLE_RATE * MCLK_MULT,
    .sample_frequency = SAMPLE_RATE,
  };
  if (es8311_init(h, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
    Serial.println("[Audio] es8311 init failed"); return;
  }
  es8311_voice_volume_set(h, 60, NULL);
  es8311_microphone_config(h, false);

  digitalWrite(AUD_PA_CTRL, LOW);   // amp OFF until a chime (saves continuous current)
  i2s_stop(I2S_PORT);               // halt MCLK/APLL between chimes (constant mA
                                    // otherwise); playMelody restarts them on demand
  s_ready = true;
  Serial.println("[Audio] ready");
}

static void playTone(int freq, int ms, float vol) {
  if (!s_ready) return;
  const int CH = 240;
  int16_t buf[CH];
  int total = SAMPLE_RATE * ms / 1000;
  int fade  = SAMPLE_RATE / 100;            // 10 ms ramps
  float ph = 0, dp = 2.0f * (float)M_PI * freq / SAMPLE_RATE;
  int done = 0; size_t bw;
  while (done < total) {
    int n = min(CH, total - done);
    for (int i = 0; i < n; i++) {
      int pos = done + i;
      float env = 1.0f;
      if (pos < fade)              env = (float)pos / fade;
      else if (pos > total - fade) env = (float)(total - pos) / fade;
      buf[i] = (int16_t)(vol * env * 26000.0f * sinf(ph));
      ph += dp; if (ph > 2 * M_PI) ph -= 2 * M_PI;
    }
    i2s_write(I2S_PORT, buf, n * 2, &bw, portMAX_DELAY);
    done += n;
  }
}

// One jingle per event, tuned to be tellable-apart on a tiny speaker:
// repetition = attention, rising = question/success, falling+low = trouble.
struct ChimeNote { uint16_t freq, ms, gapMs; };
static const ChimeNote M_NEEDS[] = {{988, 90, 70}, {988, 90, 0}};                 // B5 B5   knock-knock
static const ChimeNote M_QUEST[] = {{587, 100, 30}, {880, 200, 0}};               // D5 A5   "hm-hmm?"
static const ChimeNote M_DONE[]  = {{523, 90, 0}, {659, 90, 0}, {784, 220, 0}};   // C5 E5 G5  ta-daa!
static const ChimeNote M_THROT[] = {{392, 160, 40}, {330, 280, 0}};               // G4 E4   "uh-oh"
static const ChimeNote M_RECOV[] = {{784, 70, 0}, {1047, 140, 0}};                // G5 C6   up-flick
static const ChimeNote M_LOWB[]  = {{523, 150, 40}, {392, 300, 0}};               // C5 G4   droop

static void playMelody(const ChimeNote* n, int count, float vol) {
  i2s_start(I2S_PORT);               // clocks were stopped while idle
  digitalWrite(AUD_PA_CTRL, HIGH);   // enable amp
  delay(15);                         // codec relocks MCLK + amp settles (no pop)
  for (int i = 0; i < count; i++) {
    playTone(n[i].freq, n[i].ms, vol);
    if (n[i].gapMs) delay(n[i].gapMs);
  }
  i2s_zero_dma_buffer(I2S_PORT);
  digitalWrite(AUD_PA_CTRL, LOW);    // amp back off
  i2s_stop(I2S_PORT);                // and the clocks too
}

void chimePlay(ChimeId id) {
  if (!s_ready) return;
  switch (id) {
    case CHIME_NEEDS_YOU: playMelody(M_NEEDS, 2, 0.85f); break;
    case CHIME_QUESTION:  playMelody(M_QUEST, 2, 0.85f); break;
    case CHIME_DONE:      playMelody(M_DONE,  3, 0.85f); break;
    case CHIME_THROTTLED: playMelody(M_THROT, 2, 0.85f); break;
    case CHIME_RECOVERED: playMelody(M_RECOV, 2, 0.85f); break;
    case CHIME_LOW_BATT:  playMelody(M_LOWB,  2, 0.60f); break;
  }
}

void chime(bool workDone) { chimePlay(workDone ? CHIME_DONE : CHIME_NEEDS_YOU); }
