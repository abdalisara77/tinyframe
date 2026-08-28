/*
 * E-paper video player -- Elecrow CrowPanel ESP32 4.2" (400x300 mono, UC8176).
 *
 * Plays every baked-in clip once, in order, and then sleeps. OK (middle
 * button) cuts the current one short and moves to the next.
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
 *    glass: 100ms and 500ms both give a correct picture, 4000ms gives a solid
 *    black screen. 100 is the current default -- it plays roughly five times
 *    faster and still looks right, but see 2: looking right and being latched
 *    are not the same test, and the second one needs the cord pulled.
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
#define SETTLE_MS 100
#endif

// The resting frame is a different problem from playback, and the difference
// is not exposure -- that was the first two guesses and both were wrong.
//
// Measured on the glass: the waveform takes 1115ms to finish. Playback cuts it
// at SETTLE_MS=100 and gets away with it, because the next frame arrives and
// corrects whatever was left half done. The parked frame has no next frame, so
// cutting it early left pixels mid-swing and the picture drifted away.
//
// Letting it finish fixed the fade and exposed the next thing: the late phases
// of this LUT drive towards the opposite polarity, so a completed waveform
// lands on the inverse of the picture a 100ms cut produces. INVERT=1 was tuned
// against the cut, not against the finished state. So the resting frame is
// complemented before it is pushed, and the completed waveform then lands on
// the same picture playback was showing.
//
// (PARK_REINFORCE, which wrote the image to DTM1/0x10 as well as DTM2/0x13,
// lived here for a while on the theory that the old buffer was undefined after
// the reset. It was not the cause -- the inversion predated it -- and it broke
// the rule at the top of this file that every frame goes through the same
// registers, so it is gone.)
#ifndef PARK_INVERT
#define PARK_INVERT 1
#endif

// Drive every pixel the full distance when parking.
//
// DTM1 (0x10) is the image the controller believes it is coming FROM, DTM2
// (0x13) the one it is going TO, and the LUT picks a path per pixel from that
// pair. Playback only ever writes DTM2, which is fine there -- a pixel routed
// down a no-change path and left untouched gets another chance a tenth of a
// second later. The resting frame gets no second chance, and a pixel that was
// never driven never reaches a rail, so it leaks. That is the fade.
//
// Writing DTM1 as the exact inverse of DTM2 forces every pixel to transition,
// which is the hardest the panel can drive it. It also means the whole screen
// visibly swings on the way through, which is what a full e-paper refresh
// looks like and is why parking flashes before it settles.
//
// (An earlier version wrote DTM1 equal to DTM2 instead, to "hold" each pixel.
// That is the opposite of what is wanted here: equal means no transition,
// which means no drive, which is the thing that fades.)
#ifndef PARK_FULL_SWING
#define PARK_FULL_SWING 1
#endif

#ifndef PARK_POWER_OFF
#define PARK_POWER_OFF 1
#endif

// Longest the resting frame is allowed to take before we stop waiting on it.
// A full waveform is seconds, not milliseconds; this is only a backstop so a
// panel that never reports finished cannot hang the sketch forever.
#ifndef PARK_MAX_WAIT_MS
#define PARK_MAX_WAIT_MS 10000
#endif


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
static void pushFrame(uint32_t drawMs, bool powerDown = false,
                      bool waitDone = false, bool fullSwing = false) {
  EPD_RESET();
  delay(10);
  EPD_Init();
  delay(50);

  // Same registers and the same LUT as every other frame; this only fills in
  // DTM1, the half of the pair playback never writes, and fills it with the
  // inverse of what EPD_Display_Fast is about to put in DTM2.
  if (fullSwing) {
    EPD_WR_REG(0x10);
    for (size_t i = 0; i < FRAME_BYTES; i++) EPD_WR_DATA8((uint8_t)~frameBuf[i]);
  }

  EPD_Display_Fast(frameBuf);   // 0x50=0xD7, writes 0x13, lut_GC, 0x17/0xA5

  if (waitDone) {
    // Let the waveform actually finish, instead of cutting it at drawMs.
    //
    // EPD_ReadBusy() cannot do this for us: it breaks when BUSY reads 0, and
    // 0 is what this controller shows *while* it is busy, so it returns as the
    // refresh starts rather than when it ends. That is why drawMs behaves like
    // an exposure at all, and why 100 / 500 / 4000 give a faint, an inverted
    // and a black picture -- three points in one long multi-phase waveform,
    // not three strengths of one effect. Playback wants that cut; there is no
    // 8fps otherwise. The resting frame does not.
    //
    // Polarity is read rather than assumed: after giving BUSY a moment to
    // assert, whatever level it holds is this board's "busy", and we wait for
    // it to leave that level. The serial line says which way round it was and
    // how long the waveform really took -- if it reports ~0ms, BUSY never
    // asserted and the wait is not doing anything.
    delay(50);
    const int busyLevel = digitalRead(BUSY);
    const uint32_t t0 = millis();
    while (digitalRead(BUSY) == busyLevel && millis() - t0 < PARK_MAX_WAIT_MS) {
      delay(5);
    }
    const uint32_t waited = millis() - t0;
    Serial.printf("park: BUSY held %d for %lu ms%s\n", busyLevel,
                  (unsigned long)waited,
                  waited >= PARK_MAX_WAIT_MS ? " (TIMED OUT)" : "");
    Serial.flush();
  } else {
    settle(drawMs);
  }

  // powerDown is for the resting frame only. EPD_Sleep() is DSLP alone, which
  // latches the controller off with the booster rails still charged; POF first
  // discharges them in a defined order. It is the one thing that might stop the
  // stranded charge from bleeding back out when the supply dies, and it is NOT
  // used during playback -- POF on every frame aborts the next waveform and all
  // you get is the black/white flash.
  if (powerDown && PARK_POWER_OFF) {
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

  // frameBuf still holds what is on screen. Complement it so that a completed
  // waveform lands on that same picture rather than its inverse, then push it
  // through the identical path -- the only differences are that we wait for
  // the waveform to finish and bring the rails down deliberately afterwards.
#if PARK_INVERT
  for (size_t i = 0; i < FRAME_BYTES; i++) frameBuf[i] = (uint8_t)~frameBuf[i];
#endif
  pushFrame(0, true, true, PARK_FULL_SWING);   // drawMs unused: waitDone replaces the cut

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
  const Clip &c = CLIPS[clipIdx];
  uint32_t t0 = millis();
  uint16_t shown = 0;

  for (uint16_t i = 0; i < c.frames; i++) {
    showFrame(c, i);
    shown++;
    if (parkNow) park();          // does not return
    if (nextClip) {               // OK: cut this one short, move along
      nextClip = false;
      break;
    }
  }

  uint32_t ms = millis() - t0;
  Serial.printf("'%s' (%u/%u): %u frames, %lu ms/frame\n",
                c.name, clipIdx + 1, CLIP_COUNT, shown,
                (unsigned long)(ms / (shown ? shown : 1)));

  // One pass through every picture, in order, and then stop -- the panel is
  // bistable, so stopping leaves the last picture up rather than blank.
  //
  // clipIdx survives deep sleep, which is why setup() winds it back to 0 when
  // it is out of range: a wake starts a fresh round from the first picture
  // instead of resuming past the end of the last one.
  if (++clipIdx >= CLIP_COUNT) park();
}
