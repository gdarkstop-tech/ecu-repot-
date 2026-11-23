#include "face_painter.h"

#include <math.h>

namespace robot {

namespace {
inline int iround(float v) { return (int)lroundf(v); }
constexpr uint8_t kLower = U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT;
constexpr uint8_t kUpper = U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT;
constexpr uint8_t kLeft = U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_LOWER_LEFT;
constexpr uint8_t kRight = U8G2_DRAW_UPPER_RIGHT | U8G2_DRAW_LOWER_RIGHT;
constexpr int kW = 128, kH = 64;

void pixel(u8g2_t* g, int x, int y) {
  if (x >= 0 && y >= 0 && x < kW && y < kH) u8g2_DrawPixel(g, (u8g2_uint_t)x, (u8g2_uint_t)y);
}
}  // namespace

// ------------------------------------------------------------------ primitives
void MonoFacePainter::setRole(u8g2_t* g, Role r) { u8g2_SetDrawColor(g, r == Role::Screen ? 0 : 1); }

void MonoFacePainter::fillEllipse(u8g2_t* g, int cx, int cy, int rx, int ry, Role r, uint8_t opt) {
  if (rx < 1 || ry < 1) return;
  if (r != Role::Accent) {
    setRole(g, r);
    u8g2_DrawFilledEllipse(g, (u8g2_uint_t)cx, (u8g2_uint_t)cy, (u8g2_uint_t)rx, (u8g2_uint_t)ry, opt);
    setRole(g, Role::Feature);
    return;
  }
  // Accent: a checkerboard dither, BMO's softer tones (blush, tongue) on a 1-bit screen.
  int y0 = (opt & kUpper) ? cy - ry : cy, y1 = (opt & kLower) ? cy + ry : cy;
  setRole(g, Role::Feature);
  for (int y = y0; y <= y1; y++) {
    float k = 1.0f - (float)((y - cy) * (y - cy)) / (float)(ry * ry);
    int hw = k > 0 ? (int)(rx * sqrtf(k)) : 0;
    for (int x = cx - hw; x <= cx + hw; x++)
      if (((x + y) & 1) == 0) pixel(g, x, y);
  }
}

namespace {
inline bool inEllipse(float px, float py, float cx, float cy, float rx, float ry) {
  if (rx <= 0 || ry <= 0) return false;
  float dx = (px - cx) / rx, dy = (py - cy) / ry;
  return dx * dx + dy * dy <= 1.0f;
}
// Inside the mouth outline: upper half uses `top`, lower half `bottom` as vertical radius.
inline bool inMouth(float px, float py, float cx, float cy, float rx, float top, float bottom) {
  return inEllipse(px, py, cx, cy, rx, py < cy ? top : bottom);
}
}  // namespace

void MonoFacePainter::fillEllipseF(u8g2_t* g, float cx, float cy, float rx, float ry, Role r, int part) {
  if (rx <= 0.3f || ry <= 0.3f) return;
  setRole(g, r == Role::Screen ? Role::Screen : Role::Feature);
  int y0 = (int)floorf(cy - ry), y1 = (int)ceilf(cy + ry), x0 = (int)floorf(cx - rx), x1 = (int)ceilf(cx + rx);
  for (int y = y0; y <= y1; y++) {
    float py = y + 0.5f;
    if ((part == 1 && py > cy) || (part == 2 && py < cy)) continue;
    for (int x = x0; x <= x1; x++)
      if (inEllipse(x + 0.5f, py, cx, cy, rx, ry) && (r != Role::Accent || ((x + y) & 1) == 0)) pixel(g, x, y);
  }
  setRole(g, Role::Feature);
}

void MonoFacePainter::bmoMouth(u8g2_t* g, float cx, float cy, float rx, float top, float bottom, int teeth,
                               float tongueRx, float tongueRy) {
  int y0 = (int)floorf(cy - top), y1 = (int)ceilf(cy + bottom), x0 = (int)floorf(cx - rx), x1 = (int)ceilf(cx + rx);
  for (int y = y0; y <= y1; y++) {
    for (int x = x0; x <= x1; x++) {
      float px = x + 0.5f, py = y + 0.5f;
      if (!inMouth(px, py, cx, cy, rx, top, bottom)) continue;
      bool lit = false;
      // Teeth: the band between the top lip (a 2 px dark edge, so white teeth stand apart from
      // the lit screen) and the same curve `teeth` px lower.
      if (teeth > 0 && py < cy && inEllipse(px, py, cx, cy, rx - 2.6f, top - 2.0f) &&
          !inEllipse(px, py, cx, cy + teeth, rx - 2.6f, top - 2.0f))
        lit = true;
      // Tongue: a hump rising from the bottom, kept 1 px inside the outline, half-tone.
      if (!lit && tongueRx > 0 && py > cy && inEllipse(px, py, cx, cy + bottom, tongueRx, tongueRy) &&
          inMouth(px, py, cx, cy, rx - 1.5f, top, bottom - 1.2f))
        lit = ((x + y) & 1) == 0;
      setRole(g, lit ? Role::Screen : Role::Feature);
      pixel(g, x, y);
    }
  }
  setRole(g, Role::Feature);
}

void MonoFacePainter::thickHLine(u8g2_t* g, int x, int y, int w, int t) {
  if (w > 0 && t > 0) u8g2_DrawBox(g, (u8g2_uint_t)x, (u8g2_uint_t)y, (u8g2_uint_t)w, (u8g2_uint_t)t);
}

void MonoFacePainter::thickLine(u8g2_t* g, int x0, int y0, int x1, int y1) {
  u8g2_DrawLine(g, (u8g2_uint_t)x0, (u8g2_uint_t)y0, (u8g2_uint_t)x1, (u8g2_uint_t)y1);
  u8g2_DrawLine(g, (u8g2_uint_t)x0, (u8g2_uint_t)(y0 + 1), (u8g2_uint_t)x1, (u8g2_uint_t)(y1 + 1));
}

void MonoFacePainter::arc(u8g2_t* g, int cx, int cy, int rx, int ry, uint8_t opt) {
  // Two concentric outlines make a soft 2 px stroke.
  u8g2_DrawEllipse(g, (u8g2_uint_t)cx, (u8g2_uint_t)cy, (u8g2_uint_t)rx, (u8g2_uint_t)ry, opt);
  int dy = (opt & kLower) && !(opt & kUpper) ? 1 : ((opt & kUpper) && !(opt & kLower) ? -1 : 0);
  if (dy) u8g2_DrawEllipse(g, (u8g2_uint_t)cx, (u8g2_uint_t)(cy + dy), (u8g2_uint_t)rx, (u8g2_uint_t)ry, opt);
  else u8g2_DrawEllipse(g, (u8g2_uint_t)cx, (u8g2_uint_t)cy, (u8g2_uint_t)(rx - 1), (u8g2_uint_t)(ry > 1 ? ry - 1 : 1), opt);
}

void MonoFacePainter::micOffIcon(u8g2_t* g, int x, int y) {
  u8g2_DrawRBox(g, x + 3, y, 5, 8, 2);
  u8g2_DrawEllipse(g, x + 5, y + 5, 5, 4, kLower);
  u8g2_DrawVLine(g, x + 5, y + 10, 2);
  u8g2_DrawHLine(g, x + 2, y + 12, 7);
  int sx0 = x, sy0 = y + 13, sx1 = x + 10, sy1 = y;
  setRole(g, Role::Screen);  // outline keeps the slash readable on top of the icon
  u8g2_DrawLine(g, sx0 - 1, sy0, sx1 - 1, sy1);
  u8g2_DrawLine(g, sx0 + 2, sy0, sx1 + 2, sy1);
  setRole(g, Role::Feature);
  u8g2_DrawLine(g, sx0, sy0, sx1, sy1);
  u8g2_DrawLine(g, sx0 + 1, sy0, sx1 + 1, sy1);
}

void MonoFacePainter::wifiOffIcon(u8g2_t* g, int x, int y) {
  u8g2_DrawCircle(g, x + 5, y + 8, 7, kUpper);
  u8g2_DrawCircle(g, x + 5, y + 8, 4, kUpper);
  u8g2_DrawDisc(g, x + 5, y + 8, 1, U8G2_DRAW_ALL);
  u8g2_DrawLine(g, x, y, x + 10, y + 9);
}

// ------------------------------------------------------------------ eyes
void MonoFacePainter::eye(u8g2_t* g, int cx, int cy, const EyePose& e, bool left) {
  float s = e.scale;
  switch (e.kind) {
    case EyeKind::Dot: {  // BMO's round dot: 8 x 9 px at scale 1
      float ry = 4.5f * s * e.open;
      if (ry < 1.2f) thickHLine(g, cx - 5, cy, 10);
      else fillEllipseF(g, (float)cx, cy + 0.5f, 4.0f * s, ry, Role::Feature);
      break;
    }
    case EyeKind::Closed:
      thickHLine(g, cx - 6, cy, 13);
      break;
    case EyeKind::Arc:  // ^
      arc(g, cx, cy + 4, 7, 6, kUpper);
      break;
    case EyeKind::Lidded: {  // dot under a flat lid: proud, a bit smug
      int ry = iround(6.0f * e.open);
      if (ry <= 1) {
        thickHLine(g, cx - 7, cy, 15);
        break;
      }
      fillEllipse(g, cx, cy - 1, 6, ry, Role::Feature, kLower);
      thickHLine(g, cx - 7, cy - 2, 15);
      break;
    }
    case EyeKind::Sleepy: {  // heavy lid over the lower half
      int ry = iround(3.0f * e.open);
      thickLine(g, left ? cx - 7 : cx - 6, left ? cy + 1 : cy + 3, left ? cx + 6 : cx + 7, left ? cy + 3 : cy + 1);
      if (ry >= 1) fillEllipse(g, cx, cy + 3, 4, ry, Role::Feature, kLower);
      break;
    }
    case EyeKind::Asleep:  // relaxed closed eye
      arc(g, cx, cy - 1, 7, 3, kLower);
      break;
    case EyeKind::Cross:
      thickLine(g, cx - 5, cy - 5, cx + 5, cy + 5);
      thickLine(g, cx + 5, cy - 5, cx - 5, cy + 5);
      break;
    case EyeKind::Squint:
      thickHLine(g, cx - 6, cy - 1, 12);
      break;
    default:
      break;
  }
}

// ------------------------------------------------------------------ mouth
void MonoFacePainter::mouth(u8g2_t* g, int cx, int cy, const FacePose& p) {
  const float mx = (float)cx, my = (float)cy;  // corner line; symmetric around pixel boundary cx
  switch (p.mouth) {
    case MouthKind::Grin:     // BMO's own open mouth (proportions from the show's face)
      bmoMouth(g, mx, my, 19, 7, 9, 3, 9, 5);
      break;
    case MouthKind::BigGrin:  // wider and deeper
      bmoMouth(g, mx, my, 22, 7.5f, 12, 3, 11, 6.5f);
      break;
    case MouthKind::BigO:     // surprised: a tall round open mouth
      bmoMouth(g, mx, my + 1, 8, 7, 8, 2, 4.5f, 4);
      break;
    case MouthKind::Tongue:   // playful: open mouth with the tongue out over the lower lip
      bmoMouth(g, mx, my, 16, 6.5f, 7, 3, 0, 0);
      fillEllipseF(g, mx + 5, my + 7.5f, 5, 5, Role::Feature, 2);
      fillEllipseF(g, mx + 5, my + 7.5f, 3.6f, 3.6f, Role::Screen, 2);
      fillEllipseF(g, mx + 5, my + 7.5f, 3.6f, 3.6f, Role::Accent, 2);
      break;
    case MouthKind::Smile:    // closed smile line
      arc(g, cx, cy - 3, 11, 4, kLower);
      break;
    case MouthKind::Flat:
      thickHLine(g, cx - 8, cy, 16);
      break;
    case MouthKind::Frown:
      arc(g, cx, cy + 4, 10, 4, kUpper);
      break;
    case MouthKind::Wavy: {
      int px = cx - 10, py = cy + iround(1.5f * sinf(-10 * 0.6f));
      for (int x = -9; x <= 10; x++) {
        int y = cy + iround(1.5f * sinf(x * 0.6f));
        thickLine(g, px, py, cx + x, y);
        px = cx + x;
        py = y;
      }
      break;
    }
    case MouthKind::SmallO:  // a small open "o" (yawn, snore, wonder)
      fillEllipseF(g, mx, my + 0.5f, 4, 4.5f, Role::Feature);
      fillEllipseF(g, mx, my + 0.5f, 2, 2.5f, Role::Screen);
      break;
    case MouthKind::Smirk:
      thickLine(g, cx - 9, cy, cx + 2, cy);
      thickLine(g, cx + 2, cy, cx + 6, cy - 1);
      thickLine(g, cx + 6, cy - 1, cx + 10, cy - 4);
      break;
    case MouthKind::Zipper:
      thickHLine(g, cx - 11, cy, 23);
      for (int x = cx - 10; x <= cx + 10; x += 4) u8g2_DrawVLine(g, (u8g2_uint_t)x, (u8g2_uint_t)(cy - 2), 6);
      break;
    case MouthKind::Talk: {  // BMO talking: closed -> small -> medium -> full open mouth
      int step = iround(p.mouthOpen * 3);
      if (step <= 0) arc(g, cx, cy - 3, 11, 4, kLower);
      else if (step == 1) bmoMouth(g, mx, my, 12, 4.5f, 4, 2, 0, 0);
      else if (step == 2) bmoMouth(g, mx, my, 16, 6, 7, 2, 6, 3.5f);
      else bmoMouth(g, mx, my, 19, 7, 9, 3, 9, 5);
      break;
    }
    default:
      break;
  }
}

// ------------------------------------------------------------------ marks
void MonoFacePainter::marks(u8g2_t* g, const FacePose& p, const FaceLayout& l, int bob) {
  const int ex0 = l.eyeX[0], ex1 = l.eyeX[1], ey = l.eyeY + bob;
  uint32_t t = p.phase;
  if (p.marks & kMarkBlush) {
    fillEllipse(g, ex0 - 6, ey + 12, 6, 2, Role::Accent);
    fillEllipse(g, ex1 + 6, ey + 12, 6, 2, Role::Accent);
  }
  if (p.marks & kMarkBrows) {  // inner ends raised
    thickLine(g, ex0 - 7, ey - 9, ex0 + 5, ey - 14);
    thickLine(g, ex1 - 5, ey - 14, ex1 + 7, ey - 9);
  }
  if (p.marks & kMarkTear) {  // a drop sliding down the cheek
    int x = ex0 - 8, y = ey + 9 + (int)((t / 140) % 7);
    fillEllipse(g, x, y + 1, 2, 2, Role::Feature);
    u8g2_DrawLine(g, (u8g2_uint_t)(x - 2), (u8g2_uint_t)(y + 1), (u8g2_uint_t)x, (u8g2_uint_t)(y - 3));
    u8g2_DrawLine(g, (u8g2_uint_t)(x + 2), (u8g2_uint_t)(y + 1), (u8g2_uint_t)x, (u8g2_uint_t)(y - 3));
  }
  if (p.marks & kMarkSweat) {
    int x = ex1 + 17, y = ey - 12;
    u8g2_DrawCircle(g, (u8g2_uint_t)x, (u8g2_uint_t)(y + 4), 3, kLower);
    u8g2_DrawLine(g, (u8g2_uint_t)(x - 3), (u8g2_uint_t)(y + 4), (u8g2_uint_t)x, (u8g2_uint_t)(y - 2));
    u8g2_DrawLine(g, (u8g2_uint_t)(x + 3), (u8g2_uint_t)(y + 4), (u8g2_uint_t)x, (u8g2_uint_t)(y - 2));
  }
  if (p.marks & (kMarkQuestion | kMarkExclaim)) {
    u8g2_SetFont(g, u8g2_font_helvB12_tr);
    u8g2_DrawStr(g, (u8g2_uint_t)(ex1 + 15), (u8g2_uint_t)(ey - 1), (p.marks & kMarkQuestion) ? "?" : "!");
  }
  if (p.marks & kMarkSparkle) {
    const int pts[2][2] = {{ex0 - 21, ey - 13}, {ex1 + 21, ey - 15}};
    for (int i = 0; i < 2; i++) {
      int len = 1 + (int)(((t / 160) + i * 2) % 3);  // twinkle
      int x = pts[i][0], y = pts[i][1];
      u8g2_DrawHLine(g, (u8g2_uint_t)(x - len), (u8g2_uint_t)y, (u8g2_uint_t)(2 * len + 1));
      u8g2_DrawVLine(g, (u8g2_uint_t)x, (u8g2_uint_t)(y - len), (u8g2_uint_t)(2 * len + 1));
    }
  }
  if (p.marks & kMarkThinkDots) {
    int n = (int)((t / 350) % 4);  // bubbles appear one by one
    const int b[3][3] = {{ex1 + 12, ey - 9, 1}, {ex1 + 18, ey - 14, 2}, {ex1 + 26, ey - 19, 3}};
    for (int i = 0; i < n; i++) u8g2_DrawCircle(g, (u8g2_uint_t)b[i][0], (u8g2_uint_t)b[i][1], (u8g2_uint_t)b[i][2], U8G2_DRAW_ALL);
  }
  if (p.marks & kMarkSoundWaves) {
    int n = p.level > 0 ? 1 + p.level / 34 : 1 + (int)((t / 600) % 2);
    if (n > 3) n = 3;
    for (int i = 0; i < n; i++) {
      int r = 4 + 4 * i;
      u8g2_DrawEllipse(g, (u8g2_uint_t)(ex0 - 20), (u8g2_uint_t)(ey + 4), (u8g2_uint_t)r, (u8g2_uint_t)(r + 2), kLeft);
      u8g2_DrawEllipse(g, (u8g2_uint_t)(ex1 + 20), (u8g2_uint_t)(ey + 4), (u8g2_uint_t)r, (u8g2_uint_t)(r + 2), kRight);
    }
  }
  if (p.marks & kMarkZzz) {
    const uint8_t* fonts[3] = {u8g2_font_5x7_tr, u8g2_font_6x10_tr, u8g2_font_helvB10_tr};
    for (int i = 0; i < 3; i++) {
      float ph = (float)((t + i * 900) % 2700) / 2700.0f;
      if (ph > 0.92f) continue;
      u8g2_SetFont(g, fonts[ph < 0.33f ? 0 : (ph < 0.66f ? 1 : 2)]);
      u8g2_DrawStr(g, (u8g2_uint_t)(ex1 + 14 + (int)(ph * 14)), (u8g2_uint_t)(ey + 2 - (int)(ph * 20)), "z");
    }
  }
  if (p.marks & kMarkWifiOff) wifiOffIcon(g, 3, 2);
  if (p.marks & kMarkMicOff) micOffIcon(g, 114, 1);
}

// ------------------------------------------------------------------ face
void MonoFacePainter::draw(u8g2_t* g, const FacePose& p, const FaceLayout& l) {
  setRole(g, Role::Feature);
  int bob = iround(p.bob);
  for (int i = 0; i < 2; i++)
    eye(g, l.eyeX[i] + iround(p.eye[i].dx), l.eyeY + bob + iround(p.eye[i].dy), p.eye[i], i == 0);
  mouth(g, l.mouthX + iround(p.mouthDx), l.mouthY + bob, p);
  marks(g, p, l, bob);
}

}  // namespace robot
