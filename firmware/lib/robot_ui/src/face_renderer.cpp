#include "face_renderer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "utf8.h"

namespace robot {

namespace {

constexpr int kEyeLX = 36;
constexpr int kEyeRX = 92;
constexpr int kEyeY = 28;
constexpr float kTwoPi = 6.2831853f;
constexpr size_t kMaxLineBytes = 200;  // > max text (160 B) + separators
constexpr int kMaxReadingLines = 40;    // 600-byte answer at ~16 chars per line

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline bool reached(uint32_t now, uint32_t when) { return (int32_t)(now - when) >= 0; }

const EyeShape kBase = {36, 36, 10, 0, 0, 0, 0, 0};

void approach(float& v, float target, float a) { v += (target - v) * a; }

int textWidth(u8g2_t* g, const char* s) { return (int)u8g2_GetUTF8Width(g, s); }

// Picks the first font in the list whose rendering of s fits maxW x maxH.
const uint8_t* pickFont(u8g2_t* g, const uint8_t* const* fonts, int n, const char* s, int maxW, int maxH) {
  for (int i = 0; i < n; i++) {
    u8g2_SetFont(g, fonts[i]);
    if (textWidth(g, s) <= maxW && u8g2_GetAscent(g) <= maxH) return fonts[i];
  }
  return fonts[n - 1];
}

// Copies a UTF-8 range into a NUL-terminated buffer (for width measurement).
void copyRange(char* dst, size_t cap, const char* src, size_t len) {
  if (len >= cap) len = cap - 1;
  memcpy(dst, src, len);
  dst[len] = 0;
}

}  // namespace

// ------------------------------------------------------------------ setup
void FaceRenderer::reset(uint32_t now, uint32_t seed) {
  cur_ = kBase;
  cur_.w = 4;
  cur_.h = 3;
  rng_ = seed ? seed : 1;
  lastNow_ = now;
  blinking_ = false;
  nextBlink_ = now + 2500;
  nextSaccade_ = now + 2000;
  gazeX_ = gazeY_ = 0;
  initialized_ = true;
}

uint32_t FaceRenderer::rand32() {
  // xorshift32: deterministic, so simulator renders are reproducible.
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return rng_;
}

// ------------------------------------------------------------------ animation
bool FaceRenderer::eyesOpenFor(Expression e) const {
  return e != Expression::Sleeping && e != Expression::Error && e != Expression::Boot;
}

EyeShape FaceRenderer::targetFor(const ViewModel& vm, uint32_t) const {
  EyeShape t = kBase;
  switch (vm.expression) {
    case Expression::Idle:
    case Expression::Blink:
      t.dx = gazeX_;
      t.dy = gazeY_;
      break;
    case Expression::Happy:
      t.h = 32; t.dy = -3; t.happy = 1.0f;
      break;
    case Expression::Listening:
      t.w = 38; t.h = 36; t.r = 12; t.dy = -6;
      break;
    case Expression::Thinking:
      t.w = 34; t.h = 30; t.r = 9; t.dx = 7; t.dy = -7; t.squint = 0.35f;
      break;
    case Expression::Speaking:
      t.h = 30; t.dy = -6; t.happy = 0.35f;
      if (vm.marquee[0]) { t.h = 26; t.dy = -11; }
      break;
    case Expression::Surprised:
      t.w = 40; t.h = 40; t.r = 14; t.dy = -5;
      break;
    case Expression::Sad:
      t.w = 34; t.h = 30; t.r = 9; t.dy = 2; t.sad = 1.0f;
      break;
    case Expression::Muted:
      t.h = 22; t.r = 8; t.dy = -4;
      break;
    default:
      break;
  }
  return t;
}

float FaceRenderer::blinkFactor(uint32_t now) const {
  if (!blinking_) return 1.0f;
  uint32_t t = now - blinkStart_;
  const uint32_t kClose = 60, kOpen = 90, kCycle = kClose + kOpen + 30;
  if (doubleBlink_ && t >= kCycle) t -= kCycle;
  if (t < kClose) return 1.0f - 0.92f * (float)t / kClose;
  if (t < kClose + kOpen) return 0.08f + 0.92f * (float)(t - kClose) / kOpen;
  return 1.0f;
}

void FaceRenderer::animate(const ViewModel& vm, uint32_t now, float dt) {
  Expression e = vm.expression;

  // Explicit blink request (button press, debug command, selftest step).
  if (vm.blinkSeq != lastBlinkSeq_) {
    lastBlinkSeq_ = vm.blinkSeq;
    if (eyesOpenFor(e)) {
      blinking_ = true;
      doubleBlink_ = false;
      blinkStart_ = now;
    }
  }
  // Natural blinking whenever the eyes are open.
  uint32_t blinkLen = doubleBlink_ ? 360 : 180;
  if (blinking_ && (uint32_t)(now - blinkStart_) >= blinkLen) {
    blinking_ = false;
    doubleBlink_ = false;
  }
  if (eyesOpenFor(e) && !blinking_ && reached(now, nextBlink_)) {
    blinking_ = true;
    doubleBlink_ = (rand32() % 6) == 0;
    blinkStart_ = now;
    nextBlink_ = now + randRange(2200, 5600);
  }

  // Idle gaze wandering (small saccades), centred otherwise.
  if (e == Expression::Idle) {
    if (reached(now, nextSaccade_)) {
      if (rand32() % 3 == 0) {
        gazeX_ = 0;
        gazeY_ = 0;
      } else {
        gazeX_ = (float)((int)randRange(0, 16) - 8);
        gazeY_ = (float)((int)randRange(0, 6) - 3);
      }
      nextSaccade_ = now + randRange(1300, 3800);
    }
  } else {
    gazeX_ = 0;
    gazeY_ = 0;
  }

  EyeShape t = targetFor(vm, now);
  float a = dt / (55.0f + dt);  // ~55 ms time constant: quick but never jumps
  if (e != lastExpr_) lastExpr_ = e;
  approach(cur_.w, t.w, a);
  approach(cur_.h, t.h, a);
  approach(cur_.r, t.r, a);
  approach(cur_.dy, t.dy, a);
  approach(cur_.sad, t.sad, a);
  approach(cur_.happy, t.happy, a);
  approach(cur_.squint, t.squint, a);
  // Gaze moves a little faster, like real saccades.
  approach(cur_.dx, t.dx, dt / (35.0f + dt));
}

// ------------------------------------------------------------------ render entry
void FaceRenderer::render(u8g2_t* g, const ViewModel& vm, uint32_t now) {
  if (!initialized_) reset(now);
  float dt = clampf((float)(int32_t)(now - lastNow_), 0.0f, 100.0f);
  lastNow_ = now;

  u8g2_ClearBuffer(g);
  u8g2_SetDrawColor(g, 1);
  u8g2_SetFontMode(g, 1);
  u8g2_SetFontPosBaseline(g);

  animate(vm, now, dt);

  if (vm.overlayText[0]) {
    drawOverlayText(g, vm.overlayText, vm.overlayRtl);
    return;
  }
  if (vm.state == RobotState::Speaking && vm.readingText[0] && reached(now, vm.readingSince)) {
    drawReading(g, vm, now);
    return;
  }
  if (vm.state == RobotState::Learning && vm.contentKind != ContentKind::None) {
    drawContent(g, vm, now);
    drawStatusIcons(g, vm);
    return;
  }
  drawFace(g, vm, now);
  drawStatusIcons(g, vm);
}

// ------------------------------------------------------------------ face
void FaceRenderer::drawFace(u8g2_t* g, const ViewModel& vm, uint32_t now) {
  switch (vm.expression) {
    case Expression::Boot:
      drawBoot(g, vm, now);
      return;
    case Expression::Sleeping:
      drawClosedEyes(g, now);
      drawSleepZs(g, now);
      return;
    case Expression::Error: {
      drawErrorEyes(g, now);
      char line[48];
      snprintf(line, sizeof(line), "ERR %s", vm.errorCode[0] ? vm.errorCode : "unknown");
      u8g2_SetFont(g, u8g2_font_5x7_tr);
      int w = textWidth(g, line);
      u8g2_DrawStr(g, (u8g2_uint_t)clampi((kWidth - w) / 2, 0, kWidth), 62, line);
      return;
    }
    default:
      break;
  }

  float blink = blinkFactor(now);
  drawEye(g, kEyeLX, kEyeY, cur_, true, blink);
  drawEye(g, kEyeRX, kEyeY, cur_, false, blink);

  switch (vm.expression) {
    case Expression::Speaking:
      if (vm.marquee[0]) drawMarquee(g, vm, now, 48);
      else drawMouth(g, vm, now);
      break;
    case Expression::Listening:
      drawListeningBars(g, vm, now);
      break;
    case Expression::Thinking:
      drawThinkingDots(g, now);
      break;
    case Expression::Happy:
      u8g2_DrawEllipse(g, 64, 51, 9, 5, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      u8g2_DrawEllipse(g, 64, 52, 9, 5, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      break;
    case Expression::Sad:
      u8g2_DrawEllipse(g, 64, 58, 8, 4, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      u8g2_DrawEllipse(g, 64, 59, 8, 4, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      break;
    case Expression::Surprised:
      u8g2_DrawCircle(g, 64, 56, 5, U8G2_DRAW_ALL);
      u8g2_DrawCircle(g, 64, 56, 4, U8G2_DRAW_ALL);
      break;
    case Expression::Muted:
      drawMicIcon(g, 55, 41, true);
      break;
    default:
      break;
  }
}

void FaceRenderer::drawEye(u8g2_t* g, int cx, int cy, const EyeShape& s, bool left, float blink) {
  float hf = s.h * blink * (left ? (1.0f - s.squint * 0.35f) : 1.0f);
  int w = clampi((int)(s.w + 0.5f), 4, 60);
  int h = clampi((int)(hf + 0.5f), 2, 60);
  int x = clampi(cx + (int)lroundf(s.dx) - w / 2, 0, kWidth - w);
  int y = clampi(cy + (int)lroundf(s.dy) - h / 2, 0, kHeight - h);
  int r = clampi((int)(s.r + 0.5f), 0, (w < h ? w : h) / 2 - 1);

  if (h <= 4 || r <= 0) u8g2_DrawBox(g, x, y, w, h);
  else u8g2_DrawRBox(g, x, y, w, h, r);

  if (h <= 6) return;  // a closing lid needs no shaping
  if (s.sad > 0.02f) {
    // Outer-top eyelid droop: a black wedge over the outer corner.
    int drop = (int)(s.sad * h * 0.5f);
    u8g2_SetDrawColor(g, 0);
    if (left) u8g2_DrawTriangle(g, x - 1, y - 1, x + w, y - 1, x - 1, y + drop);
    else u8g2_DrawTriangle(g, x - 1, y - 1, x + w, y - 1, x + w, y + drop);
    u8g2_SetDrawColor(g, 1);
  }
  if (s.happy > 0.02f) {
    // Smiling eyes: carve the bottom with a tall ellipse whose sides reach the eye's lower
    // corners, leaving a crescent that tapers to points (reads as "^ ^" at 128x64).
    int ry = (int)(h * 0.97f * s.happy) + 1;
    int rx = w / 2 + 2;
    int carveTop = y + (int)(h * (1.0f - 0.55f * s.happy));
    int ecy = carveTop + ry;
    u8g2_SetDrawColor(g, 0);
    u8g2_DrawFilledEllipse(g, (u8g2_uint_t)(x + w / 2), (u8g2_uint_t)ecy, (u8g2_uint_t)rx, (u8g2_uint_t)ry,
                           U8G2_DRAW_ALL);
    u8g2_SetDrawColor(g, 1);
  }
}

void FaceRenderer::drawClosedEyes(u8g2_t* g, uint32_t now) {
  int b = (int)lroundf(1.5f * sinf(kTwoPi * (float)(now % 4000) / 4000.0f));  // slow breathing
  const int cxs[2] = {kEyeLX, kEyeRX};
  for (int i = 0; i < 2; i++) {
    for (int k = 0; k < 2; k++) {
      u8g2_DrawEllipse(g, cxs[i], kEyeY + 2 + b + k, 15, 6, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
    }
  }
}

void FaceRenderer::drawSleepZs(u8g2_t* g, uint32_t now) {
  const uint8_t* fonts[3] = {u8g2_font_5x7_tr, u8g2_font_6x10_tr, u8g2_font_helvB10_tr};
  for (int i = 0; i < 3; i++) {
    float p = (float)((now + i * 900) % 2700) / 2700.0f;
    if (p > 0.92f) continue;
    int fi = p < 0.33f ? 0 : (p < 0.66f ? 1 : 2);
    u8g2_SetFont(g, fonts[fi]);
    u8g2_DrawStr(g, (u8g2_uint_t)(104 + (int)(p * 16)), (u8g2_uint_t)(26 - (int)(p * 20)), "z");
  }
}

void FaceRenderer::drawErrorEyes(u8g2_t* g, uint32_t now) {
  int jitter = ((now % 2400) < 180) ? (int)((now / 40) % 3) - 1 : 0;
  const int cxs[2] = {kEyeLX, kEyeRX};
  for (int i = 0; i < 2; i++) {
    int cx = cxs[i] + jitter, cy = kEyeY - 2;
    for (int o = -1; o <= 1; o++) {
      u8g2_DrawLine(g, cx - 11 + o, cy - 11, cx + 11 + o, cy + 11);
      u8g2_DrawLine(g, cx + 11 + o, cy - 11, cx - 11 + o, cy + 11);
    }
  }
}

void FaceRenderer::drawBoot(u8g2_t* g, const ViewModel& vm, uint32_t now) {
  uint32_t t = now - vm.stateSince;
  EyeShape s = kBase;
  if (t < 450) {
    s.w = 4 + 32.0f * t / 450.0f;
    s.h = 3;
  } else if (t < 900) {
    s.h = 3 + 33.0f * (t - 450) / 450.0f;
  }
  float blink = 1.0f;  // one deliberate blink once the eyes are open
  if (t >= 1500 && t < 1560) blink = 1.0f - 0.92f * (t - 1500) / 60.0f;
  else if (t >= 1560 && t < 1650) blink = 0.08f + 0.92f * (t - 1560) / 90.0f;
  drawEye(g, kEyeLX, kEyeY, s, true, blink);
  drawEye(g, kEyeRX, kEyeY, s, false, blink);
  cur_ = s;  // hand over smoothly to the idle animation
  // Boot progress bar
  int pw = clampi((int)(t * 100 / 2200), 0, 100);
  u8g2_DrawFrame(g, 13, 57, 102, 5);
  if (pw > 0) u8g2_DrawBox(g, 14, 58, (u8g2_uint_t)pw, 3);
}

void FaceRenderer::drawMouth(u8g2_t* g, const ViewModel& vm, uint32_t now) {
  float level;
  if (vm.audioLevel > 0) {
    level = vm.audioLevel / 100.0f;  // real playback envelope
  } else {
    float t = now / 1000.0f;  // synthetic speech rhythm when no audio level is available
    float env = 0.5f + 0.5f * sinf(t * 9.0f) * sinf(t * 2.3f + 1.0f);
    float gate = sinf(t * 3.1f) > -0.6f ? 1.0f : 0.15f;
    level = env * gate;
  }
  level = clampf(level, 0.05f, 1.0f);
  int w = 18 + (int)(level * 6);
  int h = 2 + (int)(level * 9);
  int r = clampi(h / 2 - 1, 0, 3);
  int x = 64 - w / 2, y = 54 - h / 2;
  if (r > 0) u8g2_DrawRBox(g, x, y, w, h, r);
  else u8g2_DrawBox(g, x, y, w, h);
}

void FaceRenderer::drawListeningBars(u8g2_t* g, const ViewModel& vm, uint32_t now) {
  const int n = 7, bw = 4, gap = 3;
  static const float kShape[7] = {0.45f, 0.7f, 0.9f, 1.0f, 0.9f, 0.7f, 0.45f};
  const int total = n * bw + (n - 1) * gap;
  int x0 = 64 - total / 2;
  float t = now / 1000.0f;
  bool live = vm.micActive && vm.audioLevel > 0;
  for (int i = 0; i < n; i++) {
    float shape = kShape[i];  // centre bars taller
    float h;
    if (live) {
      float wobble = 0.65f + 0.35f * sinf(t * 17.0f + i * 1.7f);
      h = 3 + (vm.audioLevel / 100.0f) * 14.0f * shape * wobble;
    } else {
      h = 2 + 3.0f * (0.5f + 0.5f * sinf(t * 4.0f + i * 0.8f)) * shape;  // calm "I'm listening" pulse
    }
    int hi = clampi((int)h, 2, 16);
    u8g2_DrawBox(g, x0 + i * (bw + gap), 55 - hi / 2, bw, hi);
  }
}

void FaceRenderer::drawThinkingDots(u8g2_t* g, uint32_t now) {
  int active = (int)((now / 280) % 3);
  for (int i = 0; i < 3; i++) {
    int x = 54 + i * 10;
    if (i == active) u8g2_DrawDisc(g, x, 53, 3, U8G2_DRAW_ALL);
    else u8g2_DrawDisc(g, x, 56, 2, U8G2_DRAW_ALL);
  }
}

// ------------------------------------------------------------------ icons
void FaceRenderer::drawMicIcon(u8g2_t* g, int x, int y, bool large) {
  // Microphone with a bold slash = "microphone muted". large: 17x20, small: 11x14.
  int sx0, sy0, sx1, sy1;
  if (large) {
    u8g2_DrawRBox(g, x + 5, y, 7, 12, 3);
    u8g2_DrawEllipse(g, x + 8, y + 8, 7, 6, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
    u8g2_DrawBox(g, x + 8, y + 15, 2, 3);
    u8g2_DrawBox(g, x + 4, y + 18, 9, 2);
    sx0 = x + 1; sy0 = y + 20; sx1 = x + 16; sy1 = y;
  } else {
    u8g2_DrawRBox(g, x + 3, y, 5, 8, 2);
    u8g2_DrawEllipse(g, x + 5, y + 5, 5, 4, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
    u8g2_DrawVLine(g, x + 5, y + 10, 2);
    u8g2_DrawHLine(g, x + 2, y + 12, 7);
    sx0 = x; sy0 = y + 13; sx1 = x + 10; sy1 = y;
  }
  // Black outline first so the slash stays readable on top of the white icon.
  u8g2_SetDrawColor(g, 0);
  u8g2_DrawLine(g, sx0 - 1, sy0, sx1 - 1, sy1);
  u8g2_DrawLine(g, sx0 + 2, sy0, sx1 + 2, sy1);
  u8g2_SetDrawColor(g, 1);
  u8g2_DrawLine(g, sx0, sy0, sx1, sy1);
  u8g2_DrawLine(g, sx0 + 1, sy0, sx1 + 1, sy1);
}

void FaceRenderer::drawWifiOffIcon(u8g2_t* g, int x, int y) {
  // 11 x 9 Wi-Fi arcs with a slash
  u8g2_DrawCircle(g, x + 5, y + 8, 7, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  u8g2_DrawCircle(g, x + 5, y + 8, 4, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  u8g2_DrawDisc(g, x + 5, y + 8, 1, U8G2_DRAW_ALL);
  u8g2_DrawLine(g, x, y, x + 10, y + 9);
}

void FaceRenderer::drawStatusIcons(u8g2_t* g, const ViewModel& vm) {
  if (vm.state == RobotState::Startup) return;
  // Muted indicator in a corner whenever the big muted face is not showing.
  if (vm.muted && vm.expression != Expression::Muted) drawMicIcon(g, 115, 0, false);
  if (vm.offline || (vm.wifiConfigured && !vm.wifiConnected)) drawWifiOffIcon(g, 1, 0);
}

// ------------------------------------------------------------------ learning content
void FaceRenderer::drawContent(u8g2_t* g, const ViewModel& vm, uint32_t now) {
  (void)now;
  const char* v = vm.contentValue;
  const char* cap = vm.caption;
  bool capUni = utf8HasNonAscii(cap);
  int areaH = cap[0] ? (capUni ? 46 : 52) : 64;

  if (utf8HasNonAscii(v)) {
    // Arabic (or other non-Latin) value: U8g2 only has a 16 px Arabic font, so enlarge it.
    drawFittedUtf8(g, u8g2_font_unifont_t_arabic, v, 64, 1, 124, areaH - 2, 4);
  } else {
    // Bold, round letters for a child; digital digits for numbers and sums.
    static const uint8_t* const kLetter[] = {u8g2_font_fub42_tr, u8g2_font_fub35_tr, u8g2_font_fub30_tr,
                                             u8g2_font_fub25_tr};
    static const uint8_t* const kNumber[] = {u8g2_font_logisoso42_tr, u8g2_font_logisoso32_tr,
                                             u8g2_font_logisoso24_tr, u8g2_font_logisoso18_tr};
    static const uint8_t* const kMath[] = {u8g2_font_logisoso32_tr, u8g2_font_logisoso24_tr,
                                           u8g2_font_logisoso18_tr, u8g2_font_helvB14_tr};
    static const uint8_t* const kWord[] = {u8g2_font_helvB24_tr, u8g2_font_helvB18_tr, u8g2_font_helvB14_tr,
                                           u8g2_font_helvB10_tr};
    const uint8_t* const* list = kWord;
    switch (vm.contentKind) {
      case ContentKind::Letter: list = kLetter; break;
      case ContentKind::Number: list = kNumber; break;
      case ContentKind::Math: list = kMath; break;
      default: break;
    }
    const uint8_t* f = pickFont(g, list, 4, v, 124, areaH - 4);
    u8g2_SetFont(g, f);
    int w = textWidth(g, v);
    int asc = u8g2_GetAscent(g);
    int baseline = (areaH + asc) / 2;
    u8g2_DrawUTF8(g, (u8g2_uint_t)clampi((kWidth - w) / 2, 0, kWidth), (u8g2_uint_t)baseline, v);
  }

  if (cap[0]) {
    u8g2_SetFont(g, capUni ? u8g2_font_unifont_t_arabic : u8g2_font_6x10_tr);
    int w = textWidth(g, cap);
    u8g2_DrawUTF8(g, (u8g2_uint_t)clampi((kWidth - w) / 2, 0, kWidth), capUni ? 61 : 62, cap);
  }
}

// ------------------------------------------------------------------ word wrapping
// Wraps UTF-8 text into lines no wider than maxW. LTR fills lines from the first word. RTL text
// arrives already shaped and in visual order (from the gateway), so its logical start is the
// RIGHTMOST word: lines are filled from the end of the string. '\n' forces a break.
static int wrapText(u8g2_t* g, const char* text, bool rtl, int maxW, char (*lines)[kMaxLineBytes], int maxLines) {
  struct Tok { uint16_t start, len; };
  static Tok toks[200];  // static: keep the render task stack small
  int nt = 0;
  size_t n = strlen(text);
  for (size_t i = 0; i < n && nt < 200;) {
    if (text[i] == '\n') { toks[nt++] = {(uint16_t)i, 0}; i++; continue; }
    if (text[i] == ' ') { i++; continue; }
    size_t j = i;
    while (j < n && text[j] != ' ' && text[j] != '\n') j++;
    toks[nt++] = {(uint16_t)i, (uint16_t)(j - i)};
    i = j;
  }
  static char cand[kMaxLineBytes], word[kMaxLineBytes], trial[2 * kMaxLineBytes + 2];
  int nl = 0;
  cand[0] = 0;
  for (int k = 0; k < nt && nl < maxLines; k++) {
    const Tok& t = rtl ? toks[nt - 1 - k] : toks[k];
    if (t.len == 0) {
      if (cand[0]) { strcpy(lines[nl++], cand); cand[0] = 0; }
      continue;
    }
    copyRange(word, sizeof(word), text + t.start, t.len);
    if (!cand[0]) snprintf(trial, sizeof(trial), "%s", word);
    else if (rtl) snprintf(trial, sizeof(trial), "%s %s", word, cand);
    else snprintf(trial, sizeof(trial), "%s %s", cand, word);
    if (textWidth(g, trial) <= maxW || !cand[0]) {
      copyRange(cand, sizeof(cand), trial, strlen(trial));
    } else {
      strcpy(lines[nl++], cand);
      copyRange(cand, sizeof(cand), word, strlen(word));
    }
  }
  if (cand[0] && nl < maxLines) strcpy(lines[nl++], cand);
  return nl;
}

// ------------------------------------------------------------------ text overlay
void FaceRenderer::drawOverlayText(u8g2_t* g, const char* text, bool rtl) {
  bool uni = utf8HasNonAscii(text);
  u8g2_SetFont(g, uni ? u8g2_font_unifont_t_arabic : u8g2_font_6x12_tr);
  const int lineH = uni ? 16 : 12;
  static char lines[6][kMaxLineBytes];
  int nl = wrapText(g, text, rtl, 124, lines, kHeight / lineH);
  int top = (kHeight - nl * lineH) / 2;
  int asc = u8g2_GetAscent(g);
  for (int i = 0; i < nl; i++) {
    int w = textWidth(g, lines[i]);
    int x = rtl ? (kWidth - 2 - w) : (kWidth - w) / 2;
    u8g2_DrawUTF8(g, (u8g2_uint_t)clampi(x, 0, kWidth), (u8g2_uint_t)(top + i * lineH + asc + (lineH - asc) / 2), lines[i]);
  }
}

// ------------------------------------------------------------------ reading view
// The robot's answer as scrolling text (used when no speaker is installed). Whole lines only:
// partial glyph rows are hard to read on a 1.3" display. The scroll is paced so the last line
// is reached within readingDurationMs, with a pause on the first and last page.
void FaceRenderer::drawReading(u8g2_t* g, const ViewModel& vm, uint32_t now) {
  bool uni = utf8HasNonAscii(vm.readingText);
  u8g2_SetFont(g, uni ? u8g2_font_unifont_t_arabic : u8g2_font_6x12_tr);
  const int lineH = uni ? 16 : 12;
  const int visible = kHeight / lineH;  // 4 Arabic / 5 Latin lines
  static char lines[kMaxReadingLines][kMaxLineBytes];
  int nl = wrapText(g, vm.readingText, vm.readingRtl, 121, lines, kMaxReadingLines);
  int asc = u8g2_GetAscent(g);
  int first = 0;
  int top = 0;
  if (nl <= visible) {
    top = (kHeight - nl * lineH) / 2;  // short answer: centred, no scrolling
  } else {
    uint32_t t = now - vm.readingSince;
    const uint32_t holdStart = 1500, holdEnd = 1500;
    int steps = nl - visible;
    uint32_t span = vm.readingDurationMs > holdStart + holdEnd + 600 ? vm.readingDurationMs - holdStart - holdEnd : 600;
    uint32_t stepMs = span / (uint32_t)steps;
    if (stepMs < 600) stepMs = 600;
    first = t < holdStart ? 0 : clampi((int)((t - holdStart) / stepMs), 0, steps);
    // Scroll bar on the right edge.
    int barH = clampi(kHeight * visible / nl, 6, kHeight);
    int barY = (kHeight - barH) * first / steps;
    u8g2_DrawVLine(g, 126, 0, kHeight);
    u8g2_DrawBox(g, 125, (u8g2_uint_t)barY, 3, (u8g2_uint_t)barH);
  }
  for (int i = 0; i < visible && first + i < nl; i++) {
    const char* line = lines[first + i];
    int w = textWidth(g, line);
    int x = vm.readingRtl ? (122 - w) : 1;
    u8g2_DrawUTF8(g, (u8g2_uint_t)clampi(x, 0, kWidth), (u8g2_uint_t)(top + i * lineH + asc + (lineH - asc) / 2), line);
  }
}

// ------------------------------------------------------------------ subtitle ticker
void FaceRenderer::drawMarquee(u8g2_t* g, const ViewModel& vm, uint32_t now, int top) {
  bool uni = utf8HasNonAscii(vm.marquee);
  u8g2_SetFont(g, uni ? u8g2_font_unifont_t_arabic : u8g2_font_6x12_tr);
  int tw = textWidth(g, vm.marquee);
  int baseline = uni ? 62 : 60;
  if (tw <= 124) {
    u8g2_DrawUTF8(g, (u8g2_uint_t)((kWidth - tw) / 2), (u8g2_uint_t)baseline, vm.marquee);
    return;
  }
  const uint32_t pause = 900;
  const float speed = 0.030f;  // px per ms
  int travel = tw - 124 + 8;
  uint32_t cycle = pause * 2 + (uint32_t)(travel / speed);
  uint32_t t = (now - vm.marqueeSince) % cycle;
  int shift = t < pause ? 0 : clampi((int)((t - pause) * speed), 0, travel);
  int x = vm.marqueeRtl ? (124 - tw + shift) : (4 - shift);
  u8g2_SetClipWindow(g, 0, (u8g2_uint_t)top, kWidth, kHeight);
  // DrawUTF8 needs a non-negative start; skip leading glyphs that are fully off-screen.
  if (x >= 0) {
    u8g2_DrawUTF8(g, (u8g2_uint_t)x, (u8g2_uint_t)baseline, vm.marquee);
  } else {
    const char* p = vm.marquee;
    char one[5];
    while (*p) {
      size_t len = 1;
      while (p[len] && (((uint8_t)p[len]) & 0xC0) == 0x80) len++;
      copyRange(one, sizeof(one), p, len);
      int gw = textWidth(g, one);
      if (x + gw > 0) break;
      x += gw;
      p += len;
    }
    if (*p) u8g2_DrawUTF8(g, (u8g2_uint_t)clampi(x, 0, kWidth), (u8g2_uint_t)baseline, p);
  }
  u8g2_SetMaxClipWindow(g);
}

// ------------------------------------------------------------------ scaled glyphs
int drawFittedUtf8(u8g2_t* g, const uint8_t* font, const char* text, int centerX, int top, int maxW, int maxH,
                   int maxScale) {
  u8g2_SetFont(g, font);
  int w = textWidth(g, text);
  int h = u8g2_GetMaxCharHeight(g);
  if (w <= 0 || h <= 0) return 0;
  if (w > FaceRenderer::kWidth) w = FaceRenderer::kWidth;
  if (h > 24) h = 24;
  // Render once at 1x into the (still empty) top-left corner, sample it, erase it.
  static uint8_t bits[24][FaceRenderer::kWidth / 8];
  memset(bits, 0, sizeof(bits));
  u8g2_SetDrawColor(g, 0);
  u8g2_DrawBox(g, 0, 0, (u8g2_uint_t)w, (u8g2_uint_t)h);
  u8g2_SetDrawColor(g, 1);
  u8g2_SetFontPosTop(g);
  u8g2_DrawUTF8(g, 0, 0, text);
  u8g2_SetFontPosBaseline(g);
  const uint8_t* buf = u8g2_GetBufferPtr(g);
  int tileW = u8g2_GetBufferTileWidth(g);
  int minX = w, maxX = -1, minY = h, maxY = -1;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      if ((buf[(y >> 3) * tileW * 8 + x] >> (y & 7)) & 1) {
        bits[y][x >> 3] |= (uint8_t)(1 << (x & 7));
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
      }
    }
  }
  u8g2_SetDrawColor(g, 0);
  u8g2_DrawBox(g, 0, 0, (u8g2_uint_t)w, (u8g2_uint_t)h);
  u8g2_SetDrawColor(g, 1);
  if (maxX < 0) return 0;  // nothing visible (e.g. glyphs missing from the font)

  // Fit the INK box (not the font box) so short glyphs like "ب" still render large.
  int inkW = maxX - minX + 1, inkH = maxY - minY + 1;
  int scale = 1;
  for (int s = maxScale; s >= 1; s--) {
    if (inkW * s <= maxW && inkH * s <= maxH) {
      scale = s;
      break;
    }
  }
  int x0 = centerX - (inkW * scale) / 2;
  int y0 = top + (maxH - inkH * scale) / 2;
  for (int y = minY; y <= maxY; y++) {
    for (int x = minX; x <= maxX; x++) {
      if (!(bits[y][x >> 3] & (1 << (x & 7)))) continue;
      int px = x0 + (x - minX) * scale, py = y0 + (y - minY) * scale;
      if (px >= 0 && py >= 0 && px + scale <= FaceRenderer::kWidth && py + scale <= FaceRenderer::kHeight) {
        u8g2_DrawBox(g, (u8g2_uint_t)px, (u8g2_uint_t)py, (u8g2_uint_t)scale, (u8g2_uint_t)scale);
      }
    }
  }
  return scale;
}

}  // namespace robot
