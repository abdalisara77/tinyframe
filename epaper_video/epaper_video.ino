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

// The resting frame is a different problem from playback.
//
// Playback cuts every frame at 100ms mid-waveform -- that is why it is fast --
// so no pixel is ever driven all the way to a rail. Mid-clip that is fine: the
// next frame arrives a tenth of a second later. The last frame has no next
// frame, so a pixel left short of its rail leaks, and the picture drifts away.
// Letting the waveform finish is what stopped that.
//
// It also flips the polarity. INVERT=1 was tuned against the 100ms cut, and a
// completed waveform lands on the opposite of what the cut shows, so the
// resting frame is complemented to compensate.
//
// GHOSTING ON THE PARKED FRAME. Tried and ruled out, all measured on the
// glass -- longer exposures; writing DTM1 as the same image, as the inverse,
// and as the true previous frame; the panel's OTP waveform (there is none:
// BUSY released after 75ms against 1110ms).
//
// Also tried, but the test did not count: six alternating full-panel clears
// through the vendor's EPD_Clear path, white<->black full swings, and holding
// the rails 800ms past the end of each clear. EPD_Clear ends in EPD_Update ->
// EPD_ReadBusy, and EPD_ReadBusy breaks when BUSY reads 0 -- which is this
// board's *busy* level, not its idle one (see pushFrame). So it returns as the
// waveform starts and the next EPD_RESET cuts it off. Every one of those
// clears was itself a truncated waveform: it stranded charge the same way a
// 100ms playback frame does, rather than sweeping any off. See PARK_SWEEPS.
//
// Still open underneath all of it: this driver is a mix of two controllers'
// register sets -- the board is a UC8276C, EPD_Display writes images to 0x24
// which is an SSD16xx register, and EPD_Init_Fast is SSD16xx throughout. If
// the sweep is not enough, the fix is a driver written for the part.
// Let the panel go quiet before drawing the resting frame.
//
// A round ends with 72 waveforms that were each cut at 100ms, so the pixels
// are still in transit when park begins -- charge redistributing, nothing
// settled. Drawing immediately means the final waveform starts from a state
// that is still moving. Waiting first gives it something stable to drive from.
#ifndef PARK_QUIESCE_MS
#define PARK_QUIESCE_MS 5000
#endif

// Full-panel swings to both rails, run at park before the resting frame.
//
// The point is DC balance, not clearing the screen. Playback ends with charge
// stranded on the dielectric in a per-pixel pattern that mirrors the last
// frames drawn -- every waveform was cut at SETTLE_MS, so none of them ever
// reached the balancing tail. Driving every pixel rail to rail, with the
// waveform ALLOWED TO FINISH each time, is what sweeps that back off. The
// resting frame is then drawn from a uniform panel with no per-pixel history
// left in it for the image to drift back toward.
//
// These go through pushFrame(waitDone=true), which reads BUSY's polarity and
// waits for it to release. They deliberately do NOT go through EPD_Clear() --
// that path cannot wait (see above) and would strand charge instead of
// sweeping it.
//
// Counted in white/black pairs, so the net drive stays balanced. Which rail
// the last sweep lands on does not matter for correctness: the resting frame
// drives every pixel from uniform either way.
#ifndef PARK_SWEEPS
#define PARK_SWEEPS 2
#endif

#ifndef PARK_MAX_WAIT_MS
#define PARK_MAX_WAIT_MS 10000
#endif


#define FRAME_BYTES (EPD_W * EPD_H / 8)   // 400 * 300 / 8 = 15000

// Settle after the hard reset, and after the register init. Vendor values were
// 10 and 50; neither is a measurement, and together they were 60ms of every
// frame. The reset already holds RST long enough on its own, and EPD_Init is
// register writes the controller acts on immediately.
#ifndef RESET_SETTLE_MS
#define RESET_SETTLE_MS 2
#endif
#ifndef INIT_SETTLE_MS
#define INIT_SETTLE_MS 10
#endif

// Where a frame's time actually goes, filled in by pushFrame and reported once
// per clip. "484 ms/frame" does not say what to cut, and this session has
// already spent several flashes on theories that a breakdown would have
// settled immediately.
static uint32_t tReset, tInit, tData, tExpose, tSleep;

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

// The resting image, stashed before park sweeps overwrite frameBuf with solid
// rails. EXIT can land mid-clip, so re-decoding it afterwards is not an option
// -- by then the only record of what is on the glass is frameBuf itself.
static uint8_t parkBuf[FRAME_BYTES];

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
                      bool waitDone = false) {
  const uint32_t m0 = millis();
  EPD_RESET();
  delay(RESET_SETTLE_MS);
  const uint32_t m1 = millis();
  EPD_Init();
  delay(INIT_SETTLE_MS);
  const uint32_t m2 = millis();

  EPD_Display_Fast(frameBuf);   // 0x50=0xD7, writes 0x13, lut_GC, 0x17/0xA5
  const uint32_t m3 = millis();

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
  const uint32_t m4 = millis();

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

  tReset  = m1 - m0;
  tInit   = m2 - m1;
  tData   = m3 - m2;
  tExpose = m4 - m3;
  tSleep  = millis() - m4;
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

  // frameBuf still holds what is on screen. Keep it -- the sweeps below are
  // about to fill frameBuf with solid rails.
  memcpy(parkBuf, frameBuf, FRAME_BYTES);

  // Let whatever playback left in flight settle before driving anything.
  Serial.printf("park: quiescing %d ms\n", PARK_QUIESCE_MS);
  Serial.flush();
  settle(PARK_QUIESCE_MS);

  // Sweep both rails, each waveform run to completion, to pull the stranded
  // per-pixel charge back off before anything is drawn. No powerDown yet: POF
  // belongs at the very end, and firing it here would abort the next sweep.
  for (uint8_t s = 0; s < PARK_SWEEPS; s++) {
    Serial.printf("park: sweep %u/%u\n", (unsigned)(s + 1),
                  (unsigned)PARK_SWEEPS);
    Serial.flush();
    memset(frameBuf, 0xFF, FRAME_BYTES);
    pushFrame(0, false, true);
    memset(frameBuf, 0x00, FRAME_BYTES);
    pushFrame(0, false, true);
  }

  // Now the resting frame, onto a panel with no history left in it.
  // Complement it, so that a waveform allowed to finish lands on that same
  // picture instead of its inverse, then push it through the identical path --
  // the only differences are the wait and bringing the rails down deliberately
  // afterwards.
  for (size_t i = 0; i < FRAME_BYTES; i++) frameBuf[i] = (uint8_t)~parkBuf[i];
  pushFrame(0, true, true);   // drawMs unused: the wait replaces the cut

  // DO NOT pull GPIO7/GPIO41 low here. GPIO41 is the board's power-control
  // latch, not just display power: driving it low hard-powers-off the whole
  // board including the USB-serial bridge, and it needs a physical replug to
  // come back. It buys nothing anyway -- the panel is bistable and already
  // asleep, and the ESP32 is about to drop to microamps.
  // Hand the keys to the RTC domain before leaning on their pullups.
  //
  // setup() configures them with pinMode(INPUT_PULLUP), which is the digital
  // GPIO domain -- and that domain is powered down in deep sleep. Calling
  // rtc_gpio_pullup_en() without rtc_gpio_init() first leaves the pin still
  // owned by the digital side, so the pullup goes away with it and the input
  // floats. ext1 is armed ANY_LOW, so a floating pin is a wake.
  //
  // Measured: the board woke on its own with "wake cause: 3  ext1 pins: 0x20"
  // -- GPIO5, untouched -- then reset clipIdx and started redrawing picture 1.
  // A first frame half-drawn at 100ms over the parked picture is what looked
  // like the parked frame ghosting and fading.
  uint64_t mask = 0;
  for (uint8_t k = 0; k < KEY_COUNT; k++) {
    const gpio_num_t pin = (gpio_num_t)KEYS[k];
    mask |= 1ULL << KEYS[k];
    rtc_gpio_init(pin);
    rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(pin);
    rtc_gpio_pulldown_dis(pin);
  }
  // ANY_LOW wakes the instant any of these reads low, so a pin that is already
  // low when we sleep means we never sleep at all. Report them first.
  Serial.printf("keys before sleep:");
  for (uint8_t k = 0; k < KEY_COUNT; k++) {
    Serial.printf(" gpio%u=%d", KEYS[k], digitalRead(KEYS[k]));
  }
  Serial.printf("  mask=0x%llx\n", (unsigned long long)mask);
  Serial.flush();

  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();   // does not return; wake restarts at setup()
}

// ---------------------------------------------------------------- lifecycle

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nCrowPanel 4.2\" video player (invert=%d settle=%d)\n",
                INVERT, SETTLE_MS);

  // Why are we awake? A park that is immediately undone by a spurious wake
  // looks exactly like a frame that failed to latch: the picture is replaced
  // by a half-drawn first frame of the next round, which reads as fading.
  const esp_sleep_wakeup_cause_t why = esp_sleep_get_wakeup_cause();
  Serial.printf("wake cause: %d", (int)why);
  if (why == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.printf("  ext1 pins: 0x%llx",
                  (unsigned long long)esp_sleep_get_ext1_wakeup_status());
  }
  Serial.println();

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
  Serial.printf("'%s' (%u/%u): %u frames, %lu ms/frame"
                "  [reset %lu  init %lu  data %lu  expose %lu  sleep %lu]\n",
                c.name, clipIdx + 1, CLIP_COUNT, shown,
                (unsigned long)(ms / (shown ? shown : 1)),
                (unsigned long)tReset, (unsigned long)tInit,
                (unsigned long)tData, (unsigned long)tExpose,
                (unsigned long)tSleep);

  // One pass through every picture, in order, and then stop -- the panel is
  // bistable, so stopping leaves the last picture up rather than blank.
  //
  // clipIdx survives deep sleep, which is why setup() winds it back to 0 when
  // it is out of range: a wake starts a fresh round from the first picture
  // instead of resuming past the end of the last one.
  if (++clipIdx >= CLIP_COUNT) park();
}
