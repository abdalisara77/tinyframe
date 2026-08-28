/*
 * E-paper video player -- Elecrow CrowPanel ESP32 4.2" (400x300 mono, UC8176).
 *
 * Plays the baked-in clips on a loop. OK (middle button) switches clip.
 * EXIT latches the current frame and sleeps, so the cord can be pulled.
 *
 * Clips come from clips.h, which ../make_eink_video.py generates from every
 * photo in ../pictures/. It holds the frame data AND the CLIPS[] table, so
 * there is nothing clip-specific in this file and adding a photo never means
 * editing firmware. It is generated and not committed -- run the script.
 *
 * Frames are 1bpp in the panel's native layout (MSB-first, bit=1 -> white),
 * RLE-compressed unless the art is dense enough that RLE loses, in which case
 * that clip is stored raw.
 *
 * ---------------------------------------------------------------------------
 * TWO THINGS ABOUT THIS PANEL, BOTH OF WHICH DRIVE EVERY DECISION BELOW.
 *
 * 1. SETTLE_MS IS NOT A SETTLING TIME, IT IS AN EXPOSURE. Measured on the
 *    glass: 500ms gives a correct picture, 4000ms gives a solid black screen.
 *    So this LUT does not converge on the image and stop -- left running it
 *    keeps driving until everything saturates to one rail. Cutting it off is
 *    not a shortcut around a slow panel, it is the whole mechanism. There is
 *    one right exposure and both too little and too much are wrong.
 *
 * 2. Only a pixel driven fully to its rail survives losing power. Too short an
 *    exposure leaves pixels stranded mid-swing, and stranded charge bleeds back
 *    out through the panel once the supply dies -- which reads as the image
 *    inverting. So the longest exposure that has NOT started to darken is also
 *    the most unplug-proof one. That is the tuning target for SETTLE_MS.
 *
 * Every frame -- first, last, fast, slow -- goes through pushFrame() with the
 * same registers and the same LUT, differing only in exposure. There is
 * deliberately no separate "park" routine: every version of this sketch that
 * had one drew the final frame differently from the rest of playback, and every
 * one of them eventually put something wrong on the glass.
 * ---------------------------------------------------------------------------
 */

#include <Arduino.h>
#include <string.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "EPD.h"
#include "EPD_GUI.h"
#include "EPD_SPI.h"

// 1 = white artwork on black [default], 0 = black artwork on white.
#ifndef INVERT
#define INVERT 1
#endif

// Exposure per frame. Tune it on the glass: raise it until the picture starts
// going dark, then back off. Bigger = better latched = safer to unplug.
#ifndef SETTLE_MS
#define SETTLE_MS 500
#endif


#define PLAYS_BEFORE_PARK 5 // passes before the panel parks itself and sleeps

#define FRAME_BYTES (EPD_W * EPD_H / 8)   // 400 * 300 / 8 = 15000

// Buttons are active-low with pull-ups; pins from Elecrow's Example2_KEY.
#define KEY_EXIT 1
#define KEY_HOME 2
#define KEY_NEXT 4
#define KEY_OK   5
#define KEY_PRV  6
static const uint8_t KEYS[] = { KEY_EXIT, KEY_HOME, KEY_NEXT, KEY_OK, KEY_PRV };
static const uint8_t KEY_COUNT = sizeof(KEYS) / sizeof(KEYS[0]);

// Caption position/size. Vendor font sizes: 8,12,16,24,48 (glyph width size/2).
#define CAPTION_X 8
#define CAPTION_Y 8
#define CAPTION_SIZE 24

struct Clip {
  const uint8_t  *data;
  const uint32_t *offsets;
  uint16_t        frames;
  bool            rleEncoded;
  const char     *caption;
  const char     *name;
};

// Defines CLIPS[] and CLIP_COUNT. Included here, after struct Clip, because
// the generated table is an array of it.
#include "clips.h"

static uint8_t frameBuf[FRAME_BYTES];
static bool nextClip = false;
static bool parkNow  = false;
static bool keyDown[KEY_COUNT] = { false };

RTC_DATA_ATTR static uint8_t clipIdx = 0;   // survives deep sleep

// ---------------------------------------------------------------- buttons

// Edge-detect on press so holding a button does not repeat.
static void pollKeys() {
  for (uint8_t k = 0; k < KEY_COUNT; k++) {
    bool down = (digitalRead(KEYS[k]) == LOW);
    if (down && !keyDown[k]) {
      if (KEYS[k] == KEY_OK)   nextClip = true;
      if (KEYS[k] == KEY_EXIT) parkNow  = true;
    }
    keyDown[k] = down;
  }
}

static void settle(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    pollKeys();
    delay(5);
  }
}

// ---------------------------------------------------------------- panel

// The only way a frame ever reaches the panel. Same registers, same LUT, same
// everything -- drawMs is the single thing that varies, so contrast cannot
// drift between frames, or between playback and the final resting frame.
//
// drawMs is an exposure, not a timeout -- see the note at the top of the file.
// Too short and pixels are left mid-swing; too long and the panel saturates to
// black. There is no "safe" large value to fall back on.
//
// EPD_Sleep() is DSLP alone, no POF. That is deliberate: POF aborts the
// waveform outright and all you get is the opening black/white flash phases.
static void pushFrame(uint32_t drawMs, bool powerDown = false) {
  EPD_RESET();
  delay(10);
  EPD_Init();
  delay(50);
  EPD_Display_Fast(frameBuf);   // 0x50=0xD7, writes 0x13, lut_GC, 0x17/0xA5
  settle(drawMs);

  // powerDown is for the resting frame only. EPD_Sleep() is DSLP alone, which
  // latches the controller off with the booster rails still charged; POF first
  // discharges them in a defined order. It is the one thing that might stop the
  // stranded charge from bleeding back out when the supply dies, and it is NOT
  // used during playback -- POF on every frame aborts the next waveform and all
  // you get is the black/white flash.
  if (powerDown) {
    EPD_WR_REG(0x02);           // POF
    delay(200);
  }
  EPD_Sleep();                  // DSLP
}

// ---------------------------------------------------------------- frames

static void decodeFrame(const Clip &c, uint16_t idx) {
  const uint8_t *p = c.data + c.offsets[idx];
  const uint8_t *end = c.data + c.offsets[idx + 1];

  if (c.rleEncoded) {
    size_t out = 0;
    while (p + 1 < end && out < FRAME_BYTES) {
      uint16_t run = *p++;
      uint8_t val = *p++;
      if (out + run > FRAME_BYTES) run = FRAME_BYTES - out;
      memset(frameBuf + out, val, run);
      out += run;
    }
    if (out < FRAME_BYTES) memset(frameBuf + out, 0xFF, FRAME_BYTES - out);
  } else {
    memcpy(frameBuf, p, FRAME_BYTES);
  }

#if INVERT
  // bit=1 is white, so complementing the buffer swaps foreground/background.
  // This happens before the caption is drawn, so the caption is not flipped
  // with it -- it just gets drawn in the matching colour instead.
  for (size_t i = 0; i < FRAME_BYTES; i++) frameBuf[i] = (uint8_t)~frameBuf[i];
#endif

  if (c.caption && c.caption[0]) {
    // Rebind the GUI canvas onto the decoded frame; this does not clear it.
    Paint_NewImage(frameBuf, EPD_W, EPD_H, 0, WHITE);
    EPD_ShowString(CAPTION_X, CAPTION_Y, c.caption, CAPTION_SIZE,
                   INVERT ? WHITE : BLACK);
  }
}

static void showFrame(const Clip &c, uint16_t idx) {
  decodeFrame(c, idx);
  pushFrame(SETTLE_MS);
}

// ---------------------------------------------------------------- sleep

static void park() {
  Serial.println("parking -- safe to unplug");
  Serial.flush();

  // frameBuf still holds what is on screen. Same path, same exposure, so it
  // looks identical to every other frame -- the only difference is that the
  // rails come down deliberately afterwards instead of being left charged.
  pushFrame(SETTLE_MS, true);

  // DO NOT pull GPIO7/GPIO41 low here. GPIO41 is the board's power-control
  // latch, not just display power: driving it low hard-powers-off the whole
  // board including the USB-serial bridge, and it needs a physical replug to
  // come back. It buys nothing anyway -- the panel is bistable and already
  // asleep, and the ESP32 is about to drop to microamps.
  uint64_t mask = 0;
  for (uint8_t k = 0; k < KEY_COUNT; k++) {
    mask |= 1ULL << KEYS[k];
    rtc_gpio_pullup_en((gpio_num_t)KEYS[k]);
    rtc_gpio_pulldown_dis((gpio_num_t)KEYS[k]);
  }
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();   // does not return; wake restarts at setup()
}

// ---------------------------------------------------------------- lifecycle

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nCrowPanel 4.2\" video player (invert=%d settle=%d)\n",
                INVERT, SETTLE_MS);

  for (uint8_t k = 0; k < KEY_COUNT; k++) pinMode(KEYS[k], INPUT_PULLUP);

  pinMode(41, OUTPUT); digitalWrite(41, HIGH);   // V1.2A display power
  pinMode(7,  OUTPUT); digitalWrite(7,  HIGH);
  delay(100);

  EPD_GPIOInit();

  // No EPD_Clear() here on purpose. The first frame paints all 400x300 anyway,
  // and clearing on boot means a brownout reset -- exactly what pulling the
  // cord causes -- repaints the whole panel from a collapsing supply.

  if (clipIdx >= CLIP_COUNT) clipIdx = 0;
}

void loop() {
  static uint8_t passes = 0;
  const Clip &c = CLIPS[clipIdx];
  uint32_t t0 = millis();

  for (uint16_t i = 0; i < c.frames; i++) {
    showFrame(c, i);
    if (parkNow) park();          // does not return
    if (nextClip) {
      nextClip = false;
      clipIdx = (clipIdx + 1) % CLIP_COUNT;
      passes = 0;
      return;                     // restart loop() on the new clip
    }
  }

  uint32_t ms = millis() - t0;
  passes++;
  Serial.printf("'%s' pass %u/%d: %u frames, %lu ms/frame\n",
                c.name, passes, PLAYS_BEFORE_PARK, c.frames,
                (unsigned long)(ms / c.frames));

  if (passes >= PLAYS_BEFORE_PARK) park();
}
