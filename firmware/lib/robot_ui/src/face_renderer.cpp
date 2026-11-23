#include "face_renderer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "utf8.h"

namespace robot {

namespace {

constexpr size_t kMaxLineBytes = 200;  // > max text (160 B) + separators
constexpr int kMaxReadingLines = 40;    // 600-byte answer at ~16 chars per line

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline bool reached(uint32_t now, uint32_t when) { return (int32_t)(now - when) >= 0; }

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
  anim_.reset(now, seed);
  initialized_ = true;
}

// ------------------------------------------------------------------ render entry
void FaceRenderer::render(u8g2_t* g, const ViewModel& vm, uint32_t now) {
  if (!initialized_) reset(now);
  u8g2_ClearBuffer(g);
  u8g2_SetDrawColor(g, 1);
  u8g2_SetFontMode(g, 1);
  u8g2_SetFontPosBaseline(g);

  pose_ = anim_.update(vm, now);  // the face keeps living even behind other screens

  if (vm.overlayText[0]) {
    drawOverlayText(g, vm.overlayText, vm.overlayRtl);
  } else if (vm.state == RobotState::Speaking && vm.readingText[0] && reached(now, vm.readingSince)) {
    drawReading(g, vm, now);
  } else if (vm.state == RobotState::Learning && vm.contentKind != ContentKind::None) {
    drawContent(g, vm, now);
    drawStatusIcons(g, vm);
  } else if (vm.expression == Expression::Boot) {
    drawBoot(g, vm, now);
    return;  // the boot animation handles light/dark itself
  } else {
    drawFace(g, vm, now);
    drawStatusIcons(g, vm);
    finish(g, pose_.darkScreen);
    return;
  }
  finish(g, false);
}

// Light screen: invert the finished frame so features are dark on a lit screen.
void FaceRenderer::finish(u8g2_t* g, bool darkScreen) {
  if (!light_ || darkScreen) return;
  uint8_t* buf = u8g2_GetBufferPtr(g);
  size_t n = (size_t)u8g2_GetBufferTileWidth(g) * u8g2_GetBufferTileHeight(g) * 8;
  for (size_t i = 0; i < n; i++) buf[i] ^= 0xFF;
}

// ------------------------------------------------------------------ face
void FaceRenderer::drawFace(u8g2_t* g, const ViewModel& vm, uint32_t now) {
  painter_.draw(g, pose_, layout_);
  if (vm.expression == Expression::Error) {
    char line[48];
    snprintf(line, sizeof(line), "ERR %s", vm.errorCode[0] ? vm.errorCode : "unknown");
    drawLabel(g, line);
  } else if (vm.state == RobotState::Speaking && vm.marquee[0]) {
    drawMarquee(g, vm, now, 52);  // a short subtitle (debug "say", or a short label)
  }
}

// A short label in small type at the bottom (error codes, short status words).
void FaceRenderer::drawLabel(u8g2_t* g, const char* text) {
  u8g2_SetFont(g, u8g2_font_5x7_tr);
  int w = textWidth(g, text);
  u8g2_DrawStr(g, (u8g2_uint_t)clampi((kWidth - w) / 2, 0, kWidth), 62, text);
}

// Retro power-on: a dot, a scan line, the screen opening, then the face with loading blocks.
void FaceRenderer::drawBoot(u8g2_t* g, const ViewModel& vm, uint32_t now) {
  uint32_t t = now - vm.stateSince;
  // Draw in physical terms: lit(true) is a lit pixel whatever the screen style.
  const uint8_t litColor = light_ ? 0 : 1, darkColor = light_ ? 1 : 0;
  if (t < 950) {
    u8g2_SetDrawColor(g, darkColor);
    u8g2_DrawBox(g, 0, 0, kWidth, kHeight);
    u8g2_SetDrawColor(g, litColor);
    if (t < 300) {
      u8g2_DrawDisc(g, 64, 32, (u8g2_uint_t)(1 + t / 100), U8G2_DRAW_ALL);
    } else if (t < 650 || !light_) {
      int w = clampi((int)((t - 300) * kWidth / 350), 2, kWidth);
      u8g2_DrawBox(g, (u8g2_uint_t)((kWidth - w) / 2), 31, (u8g2_uint_t)w, 2);
    } else {
      int h = clampi((int)((t - 650) * kHeight / 300), 2, kHeight);
      u8g2_DrawBox(g, 0, (u8g2_uint_t)((kHeight - h) / 2), kWidth, (u8g2_uint_t)h);
    }
  } else {
    FacePose p = pose_;
    float open = t < 1250 ? (float)(t - 950) / 300.0f : 1.0f;  // eyes open
    p.eye[0].open = p.eye[1].open = open;
    p.mouth = MouthKind::Smile;
    p.marks = 0;
    painter_.draw(g, p, layout_);
    for (int i = 0; i < 3; i++) {  // loading blocks
      int x = 55 + i * 7;
      bool filled = t >= 1250 + (uint32_t)i * 300;
      if (filled) u8g2_DrawBox(g, (u8g2_uint_t)x, 57, 4, 4);
      else u8g2_DrawFrame(g, (u8g2_uint_t)x, 57, 4, 4);
    }
    finish(g, false);
    return;
  }
  u8g2_SetDrawColor(g, 1);
}

// ------------------------------------------------------------------ icons
void FaceRenderer::drawStatusIcons(u8g2_t* g, const ViewModel& vm) {
  if (vm.state == RobotState::Startup) return;
  u8g2_SetDrawColor(g, 1);
  // Muted indicator in a corner whenever the muted face (zipped mouth + icon) is not showing.
  if (vm.muted && !(pose_.marks & kMarkMicOff)) MonoFacePainter::micOffIcon(g, 114, 1);
  if ((vm.offline || (vm.wifiConfigured && !vm.wifiConnected)) && !(pose_.marks & kMarkWifiOff))
    MonoFacePainter::wifiOffIcon(g, 3, 2);
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
