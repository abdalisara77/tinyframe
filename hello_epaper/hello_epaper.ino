/*
 * Hello World + bouncing emoticon
 * Elecrow CrowPanel ESP32 4.2" E-Paper HMI  (ESP32-S3-WROOM-1-N8R8, 400x300 mono)
 *
 * Display pins (identical on every board revision):
 *   SCK 12   MOSI 11   RST 47   DC 46   CS 45   BUSY 48
 * Panel power enable: GPIO 7  (+ GPIO 41 on the V1.2A revision)
 *
 * Elecrow shipped two revisions with DIFFERENT display controllers, which need
 * different init/update calls. Select with -DGREEN_STICKER_REV=<0|1>:
 *   1 = V1.2A, green circular sticker on the back -> UC8176-family
 *   0 = V1.0 / V1.2, no sticker                   -> SSD1683
 */

#include <Arduino.h>
#include "EPD.h"
#include "EPD_GUI.h"

#ifndef GREEN_STICKER_REV
#define GREEN_STICKER_REV 1
#endif

#define EPD_PWR_PIN   7
#define EPD_PWR2_PIN  41   // V1.2A only; harmless (unused pin) on older boards

// 400 * 300 / 8 = 15000 bytes of 1bpp framebuffer
static uint8_t Image_BW[15000];

// ASCII emoticons -- the vendor font is ASCII-only, so no unicode here.
static const char *kFaces[] = {
  "(^_^)",
  "(>_<)",
  "(^o^)",
  "(o_O)",
  "(=^.^=)",
  "\\(^o^)/",
  "(~_~)",
  "(*_*)",
};
static const int kFaceCount = sizeof(kFaces) / sizeof(kFaces[0]);

static const uint16_t kHeaderSize = 24;   // font height; glyph width = size/2
static const uint16_t kFaceSize   = 48;
static const char    *kHeader     = "Hello, World!";

static int lastFace = -1;
static uint32_t frame = 0;

// Re-init the panel and push the framebuffer. The panel is put to sleep after
// every update, so each frame has to bring it back up first.
static void pushFrame() {
#if GREEN_STICKER_REV
  EPD_RESET();
  delay(100);
  EPD_Init();
  delay(300);
  EPD_Display_Fast(Image_BW);
#else
  EPD_Init_Fast(Fast_Seconds_1_5s);
  EPD_Display_Part(0, 0, EPD_W, EPD_H, Image_BW);
#endif
  delay(500);
  EPD_Sleep();
}

static void drawScene(uint16_t faceX, uint16_t faceY, const char *face) {
  Paint_NewImage(Image_BW, EPD_W, EPD_H, 0, WHITE);
  EPD_Full(WHITE);

  // Centred header + a rule underneath it
  uint16_t headerW = strlen(kHeader) * (kHeaderSize / 2);
  EPD_ShowString((EPD_W - headerW) / 2, 12, kHeader, kHeaderSize, BLACK);
  EPD_DrawLine(40, 48, EPD_W - 40, 48, BLACK);

  EPD_ShowString(faceX, faceY, face, kFaceSize, BLACK);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("CrowPanel 4.2\" e-paper: hello world");
  Serial.printf("build: GREEN_STICKER_REV=%d\n", GREEN_STICKER_REV);

  // Panel power. GPIO41 is the extra rail on V1.2A; driving it on older
  // boards is a no-op, so we can set both and stay revision-agnostic.
  pinMode(EPD_PWR2_PIN, OUTPUT);
  digitalWrite(EPD_PWR2_PIN, HIGH);
  pinMode(EPD_PWR_PIN, OUTPUT);
  digitalWrite(EPD_PWR_PIN, HIGH);
  delay(100);

  EPD_GPIOInit();
  EPD_Clear();
  delay(500);

  randomSeed(esp_random());

  // First frame, centred.
  lastFace = 0;
  drawScene((EPD_W - strlen(kFaces[0]) * (kFaceSize / 2)) / 2, 140, kFaces[0]);
  pushFrame();
  Serial.println("frame 0 pushed (centred)");
}

void loop() {
  delay(3000);   // hold each pose a few seconds

  // Pick a different face than last time
  int f;
  do {
    f = random(kFaceCount);
  } while (kFaceCount > 1 && f == lastFace);
  lastFace = f;

  const char *face = kFaces[f];
  uint16_t faceW = strlen(face) * (kFaceSize / 2);

  // Keep the face fully on screen and clear of the header/rule
  uint16_t x = random(0, EPD_W - faceW + 1);
  uint16_t y = random(60, EPD_H - kFaceSize + 1);

  drawScene(x, y, face);
  pushFrame();

  Serial.printf("frame %lu: %-8s at (%u,%u)\n", (unsigned long)++frame, face, x, y);
}
