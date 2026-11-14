// OLED face + screen renderer for a 128x64 monochrome display.
//
// Uses only the U8g2 C API, so the exact same drawing code runs on the ESP32-S3 and in the
// host simulator (which renders reference images for review and tests).
//
// The renderer owns purely visual animation state (eye tweening, blinks, gaze, tickers).
// What to show is decided by Robot and delivered as a ViewModel each frame.
#pragma once
#include <stdint.h>
#include <u8g2.h>

#include "view_model.h"

namespace robot {

struct EyeShape {
  float w, h, r;      // size and corner radius
  float dx, dy;       // offset (gaze / posture)
  float sad;          // 0..1 outer-top eyelid droop
  float happy;        // 0..1 bottom crescent carve (smiling eyes)
  float squint;       // 0..1 extra height reduction of the LEFT eye (thinking)
};

class FaceRenderer {
 public:
  static constexpr int kWidth = 128;
  static constexpr int kHeight = 64;

  void reset(uint32_t now, uint32_t seed = 0x1234567u);

  // Draws one complete frame into the U8g2 buffer. The caller sends it to the display.
  void render(u8g2_t* g, const ViewModel& vm, uint32_t now);

 private:
  // animation
  void animate(const ViewModel& vm, uint32_t now, float dt);
  EyeShape targetFor(const ViewModel& vm, uint32_t now) const;
  bool eyesOpenFor(Expression e) const;
  float blinkFactor(uint32_t now) const;
  uint32_t rand32();
  uint32_t randRange(uint32_t lo, uint32_t hi) { return lo + rand32() % (hi - lo + 1); }

  // face parts
  void drawFace(u8g2_t* g, const ViewModel& vm, uint32_t now);
  void drawEye(u8g2_t* g, int cx, int cy, const EyeShape& s, bool left, float blink);
  void drawClosedEyes(u8g2_t* g, uint32_t now);
  void drawErrorEyes(u8g2_t* g, uint32_t now);
  void drawBoot(u8g2_t* g, const ViewModel& vm, uint32_t now);
  void drawMouth(u8g2_t* g, const ViewModel& vm, uint32_t now);
  void drawListeningBars(u8g2_t* g, const ViewModel& vm, uint32_t now);
  void drawThinkingDots(u8g2_t* g, uint32_t now);
  void drawSleepZs(u8g2_t* g, uint32_t now);
  void drawStatusIcons(u8g2_t* g, const ViewModel& vm);
  void drawMicIcon(u8g2_t* g, int x, int y, bool large);
  void drawWifiOffIcon(u8g2_t* g, int x, int y);

  // screens
  void drawContent(u8g2_t* g, const ViewModel& vm, uint32_t now);
  void drawOverlayText(u8g2_t* g, const char* text, bool rtl);
  void drawMarquee(u8g2_t* g, const ViewModel& vm, uint32_t now, int top);

  EyeShape cur_{};
  bool initialized_ = false;
  uint32_t lastNow_ = 0;
  uint32_t rng_ = 1;

  // blink
  uint32_t blinkStart_ = 0;
  bool blinking_ = false;
  bool doubleBlink_ = false;
  uint32_t nextBlink_ = 0;
  uint32_t lastBlinkSeq_ = 0;

  // gaze (idle saccades)
  float gazeX_ = 0, gazeY_ = 0;
  uint32_t nextSaccade_ = 0;

  Expression lastExpr_ = Expression::Boot;
};

// Draws UTF-8 text enlarged by the largest integer factor (<= maxScale) whose INK box fits
// maxW x maxH, centred at centerX within [top, top+maxH]. Used for big Arabic glyphs because
// U8g2's Arabic font only exists at 16 px. Must be called before anything else is drawn in the
// top-left 128x24 area of the buffer (it uses it as scratch). Returns the scale used (0 = nothing).
int drawFittedUtf8(u8g2_t* g, const uint8_t* font, const char* text, int centerX, int top, int maxW, int maxH,
                   int maxScale);

}  // namespace robot
