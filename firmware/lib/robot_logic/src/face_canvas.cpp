#include "face_canvas.h"

#include <math.h>

namespace robot {

namespace {

constexpr float kPi = 3.14159265f;

inline bool inEllipse(float px, float py, float cx, float cy, float rx, float ry) {
  if (rx <= 0 || ry <= 0) return false;
  float dx = (px - cx) / rx, dy = (py - cy) / ry;
  return dx * dx + dy * dy <= 1.0f;
}

inline bool inPart(float px, float py, float cx, float cy, int part) {
  switch (part) {
    case 1: return py <= cy;
    case 2: return py >= cy;
    case 3: return px <= cx;
    case 4: return px >= cx;
    default: return true;
  }
}

inline float segDist2(float px, float py, float x0, float y0, float x1, float y1) {
  float vx = x1 - x0, vy = y1 - y0, wx = px - x0, wy = py - y0;
  float l2 = vx * vx + vy * vy;
  float t = l2 > 0 ? (wx * vx + wy * vy) / l2 : 0;
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  float dx = px - (x0 + t * vx), dy = py - (y0 + t * vy);
  return dx * dx + dy * dy;
}

// BMO's mouth outline: a flat-topped, deeper-bottomed shape with blunt, rounded ends
// (superellipse: exponent 3.2 above the corner line, 2.3 below). Returns the half-width of the
// shape on the row py, or -1 when the row misses it — two powf per ROW, not per pixel, so the
// mouth costs the ESP32 little more than an ellipse.
inline float mouthHalfWidth(float py, float cy, float rx, float top, float bottom) {
  if (rx <= 0 || top <= 0 || bottom <= 0) return -1;
  bool upper = py < cy;
  float ny = fabsf(py - cy) / (upper ? top : bottom);
  if (ny > 1) return -1;
  float e = upper ? 3.2f : 2.3f;
  return rx * powf(1.0f - powf(ny, e), 1.0f / e);
}

bool inPolygon(const float* xy, int n, float px, float py) {
  bool in = false;
  for (int i = 0, j = n - 1; i < n; j = i++) {
    float xi = xy[2 * i], yi = xy[2 * i + 1], xj = xy[2 * j], yj = xy[2 * j + 1];
    if (((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / (yj - yi) + xi)) in = !in;
  }
  return in;
}

}  // namespace

// ------------------------------------------------------------------ tones
Rgb bmoColor(Tone t) {
  switch (t) {
    case Tone::Screen: return {215, 247, 207};  // sampled from BMO's face
    case Tone::Ink: return {14, 16, 14};
    case Tone::Lip: return {0, 74, 28};
    case Tone::Mouth: return {19, 139, 62};
    case Tone::Teeth: return {255, 255, 255};
    case Tone::Tongue: return {89, 200, 106};
    case Tone::Blush: return {255, 156, 170};
    case Tone::Heart: return {232, 62, 96};
    case Tone::Star: return {255, 196, 40};
    case Tone::Tear: return {112, 190, 255};
    case Tone::Glint: return {255, 255, 255};
    case Tone::Hollow: return {19, 139, 62};
    default: return {255, 0, 255};
  }
}

uint8_t monoValue(Tone t, int x, int y) {
  switch (t) {
    case Tone::Screen: case Tone::Teeth: case Tone::Glint: case Tone::Hollow:
      return 0;
    case Tone::Tongue: case Tone::Blush: case Tone::Tear:
      return ((x + y) & 1) ? 1 : 0;  // half-tone: reads as a middle shade on the OLED
    default:
      return 1;
  }
}

void RgbSurface::plot(int x, int y, Tone t) {
  Rgb c = bmoColor(t);
  if (dim_) c = {(uint8_t)(c.r * 3 / 10), (uint8_t)(c.g * 3 / 10), (uint8_t)(c.b * 3 / 10)};
  uint8_t* p = buf_ + 3 * (y * w_ + x);
  p[0] = c.r;
  p[1] = c.g;
  p[2] = c.b;
}

void RgbSurface::clear(Tone t) {
  for (int y = 0; y < h_; y++)
    for (int x = 0; x < w_; x++) plot(x, y, t);
}

// ------------------------------------------------------------------ primitives
// Visits every device pixel whose centre lies in the box (face-space units) and paints the tone
// returned by toneAt(face-space x, y) — a negative value leaves the pixel alone.
template <class F>
void FacePainter::paint(float x0, float y0, float x1, float y1, F toneAt) {
  int X0 = (int)floorf(x0 * k_), X1 = (int)ceilf(x1 * k_), Y0 = (int)floorf(y0 * k_), Y1 = (int)ceilf(y1 * k_);
  if (X0 < 0) X0 = 0;
  if (Y0 < 0) Y0 = 0;
  if (X1 > s_.width() - 1) X1 = s_.width() - 1;
  if (Y1 > s_.height() - 1) Y1 = s_.height() - 1;
  for (int Y = Y0; Y <= Y1; Y++) {
    float py = (Y + 0.5f) / k_;
    for (int X = X0; X <= X1; X++) {
      int t = toneAt((X + 0.5f) / k_, py);
      if (t >= 0) s_.plot(X, Y, (Tone)t);
    }
  }
}

void FacePainter::ellipse(float cx, float cy, float rx, float ry, Tone t, int part) {
  if (rx <= 0.2f || ry <= 0.2f) return;
  paint(cx - rx, cy - ry, cx + rx, cy + ry, [&](float px, float py) {
    return inEllipse(px, py, cx, cy, rx, ry) && inPart(px, py, cx, cy, part) ? (int)t : -1;
  });
}

void FacePainter::ring(float cx, float cy, float rx, float ry, float w, Tone t, int part) {
  float h = w / 2;
  paint(cx - rx - h, cy - ry - h, cx + rx + h, cy + ry + h, [&](float px, float py) {
    return inEllipse(px, py, cx, cy, rx + h, ry + h) && !inEllipse(px, py, cx, cy, rx - h, ry - h) &&
                   inPart(px, py, cx, cy, part)
               ? (int)t
               : -1;
  });
}

void FacePainter::line(float x0, float y0, float x1, float y1, float w, Tone t) {
  float h = w / 2, h2 = h * h;
  paint(fminf(x0, x1) - h, fminf(y0, y1) - h, fmaxf(x0, x1) + h, fmaxf(y0, y1) + h,
        [&](float px, float py) { return segDist2(px, py, x0, y0, x1, y1) <= h2 ? (int)t : -1; });
}

void FacePainter::rect(float x, float y, float w, float h, Tone t) {
  paint(x, y, x + w, y + h, [&](float px, float py) {
    return px >= x && px <= x + w && py >= y && py <= y + h ? (int)t : -1;
  });
}

void FacePainter::polygon(const float* xy, int n, Tone t) {
  float x0 = xy[0], x1 = xy[0], y0 = xy[1], y1 = xy[1];
  for (int i = 1; i < n; i++) {
    x0 = fminf(x0, xy[2 * i]);
    x1 = fmaxf(x1, xy[2 * i]);
    y0 = fminf(y0, xy[2 * i + 1]);
    y1 = fmaxf(y1, xy[2 * i + 1]);
  }
  paint(x0, y0, x1, y1, [&](float px, float py) { return inPolygon(xy, n, px, py) ? (int)t : -1; });
}

void FacePainter::mouth(float cx, float cy, float rx, float top, float bottom, float teeth, float tongueRx,
                        float tongueRy) {
  // A 2 px dark edge on the 1-bit SH1106 keeps white teeth apart from the lit screen; a colour
  // display separates them by colour, so its lip is BMO-thin.
  const float lip = k_ >= 1.5f ? 0.9f : 1.6f;
  const float tx = rx - lip - 1.5f, tt = top - lip;  // teeth: an even band under the top lip
  float rowY = -1e9f, wOut = -1, wIn = -1, wTeeth = -1, wGum = -1, wTongue = -1;
  paint(cx - rx, cy - top, cx + rx, cy + bottom, [&](float px, float py) {
    if (py != rowY) {  // paint() walks row by row
      rowY = py;
      wOut = mouthHalfWidth(py, cy, rx, top, bottom);
      wIn = mouthHalfWidth(py, cy, rx - lip, top - lip, bottom - lip);
      wTeeth = teeth > 0 ? mouthHalfWidth(py, cy, tx, tt, 0.01f) : -1;
      wGum = teeth > 0 ? mouthHalfWidth(py, cy + teeth, tx, tt, 0.01f) : -1;
      wTongue = mouthHalfWidth(py, cy, rx - lip - 0.5f, top, bottom - lip - 0.3f);
    }
    float ax = fabsf(px - cx);
    if (ax > wOut) return -1;
    if (ax > wIn) return (int)Tone::Lip;
    if (ax <= wTeeth && ax > wGum) return (int)Tone::Teeth;
    if (tongueRx > 0 && py > cy && ax <= wTongue && inEllipse(px, py, cx, cy + bottom, tongueRx, tongueRy))
      return (int)Tone::Tongue;
    return (int)Tone::Mouth;
  });
}

void FacePainter::heart(float cx, float cy, float size, Tone t) {
  float r = 0.30f * size;
  paint(cx - size / 2 - 1, cy - size / 2 - 1, cx + size / 2 + 1, cy + size / 2 + 1, [&](float px, float py) {
    bool lobes = inEllipse(px, py, cx - 0.24f * size, cy - 0.12f * size, r, r) ||
                 inEllipse(px, py, cx + 0.24f * size, cy - 0.12f * size, r, r);
    float tri[6] = {cx - 0.52f * size, cy - 0.04f * size, cx + 0.52f * size, cy - 0.04f * size, cx, cy + 0.52f * size};
    return lobes || inPolygon(tri, 3, px, py) ? (int)t : -1;
  });
}

void FacePainter::star(float cx, float cy, float r, float turn, Tone t) {
  float xy[20];
  for (int i = 0; i < 10; i++) {
    float a = -kPi / 2 + turn + i * kPi / 5;
    float rr = (i % 2) ? r * 0.45f : r;
    xy[2 * i] = cx + rr * cosf(a);
    xy[2 * i + 1] = cy + rr * sinf(a);
  }
  polygon(xy, 10, t);
}

void FacePainter::glyphZ(float x, float y, float h) {
  float w = h * 0.8f, s = h > 4 ? 1.3f : 1.0f;
  line(x, y - h, x + w, y - h, s, Tone::Ink);
  line(x + w, y - h, x, y, s, Tone::Ink);
  line(x, y, x + w, y, s, Tone::Ink);
}

// ------------------------------------------------------------------ eyes
void FacePainter::eye(float cx, float cy, const EyePose& e, bool left, uint32_t phase) {
  float s = e.scale;
  float dir = left ? 1.0f : -1.0f;  // +1 points toward the nose for the left eye
  switch (e.kind) {
    case EyeKind::Dot: {  // BMO's round dot (proportions measured from the show's face). On the 1-bit
      // SH1106 a dot this small rounds to a diamond, so it is drawn a pixel larger there.
      const bool mono = k_ < 1.5f;
      float ry = (mono ? 4.6f : 4.1f) * s * e.open;
      if (ry < 1.2f) rect(cx - 5, cy, 10, 2, Tone::Ink);
      else ellipse(cx, cy + 0.5f, (mono ? 4.1f : 3.6f) * s, ry, Tone::Ink);
      break;
    }
    case EyeKind::Closed:
      rect(cx - 6, cy, 13, 2, Tone::Ink);
      break;
    case EyeKind::Arc:  // ^
      ring(cx, cy + 4, 7, 5.5f, 2, Tone::Ink, 1);
      break;
    case EyeKind::Lidded: {
      float ry = 6.0f * e.open;
      if (ry <= 1) rect(cx - 7, cy, 15, 2, Tone::Ink);
      else ellipse(cx, cy - 1, 6, ry, Tone::Ink, 2);
      rect(cx - 7, cy - 2, 15, 2, Tone::Ink);
      break;
    }
    case EyeKind::Sleepy: {
      float ry = 3.0f * e.open;
      line(cx - 7 * dir, cy + 1, cx + 6 * dir, cy + 3, 2, Tone::Ink);
      if (ry >= 1) ellipse(cx, cy + 3, 4, ry, Tone::Ink, 2);
      break;
    }
    case EyeKind::Asleep:
      ring(cx, cy - 1, 7, 3, 2, Tone::Ink, 2);
      break;
    case EyeKind::Cross:
      line(cx - 5, cy - 5, cx + 5, cy + 5, 2, Tone::Ink);
      line(cx + 5, cy - 5, cx - 5, cy + 5, 2, Tone::Ink);
      break;
    case EyeKind::Squint:
      rect(cx - 6, cy - 1, 12, 2, Tone::Ink);
      break;
    case EyeKind::Heart: {  // a heartbeat: 1 px bigger twice a second
      float beat = (phase % 500) < 120 ? 1.5f : 0.0f;
      heart(cx, cy + 1, 13 + beat, Tone::Heart);
      break;
    }
    case EyeKind::Star:
      star(cx, cy + 1, 7, 0.15f * sinf(phase / 300.0f), Tone::Star);
      break;
    case EyeKind::Spiral: {  // turns slowly
      float turn = (phase % 1500) / 1500.0f * 2 * kPi * (left ? 1 : -1);
      float px = cx, py = cy;
      for (int i = 1; i <= 28; i++) {
        float a = i * 0.36f, r = 0.25f * a * 2.1f;
        float x = cx + r * cosf(a + turn), y = cy + r * sinf(a + turn);
        line(px, py, x, y, 1.5f, Tone::Ink);
        px = x;
        py = y;
      }
      break;
    }
    case EyeKind::Squeeze:  // > <
      line(cx - 5 * dir, cy - 4, cx + 4 * dir, cy, 2, Tone::Ink);
      line(cx + 4 * dir, cy, cx - 5 * dir, cy + 4, 2, Tone::Ink);
      break;
    case EyeKind::Grumpy: {  // dot under a lid sloping down toward the nose
      float ry = 4.0f * e.open;
      float x0 = cx - 6 * dir, y0 = cy - 2, x1 = cx + 6 * dir, y1 = cy + 1.5f;
      if (ry >= 1) {
        paint(cx - 5, cy - 4, cx + 5, cy + 6, [&](float px, float py) {
          if (!inEllipse(px, py, cx, cy + 1.5f, 4.2f, ry)) return -1;
          float lidY = y0 + (y1 - y0) * (px - x0) / (x1 - x0);  // y of the lid line at px
          return py > lidY ? (int)Tone::Ink : -1;
        });
      }
      line(x0, y0, x1, y1, 2, Tone::Ink);
      break;
    }
    case EyeKind::SideEye: {  // half-lidded, pupil to the side
      float ry = 5.0f * e.open;
      if (ry > 1) ellipse(cx + 2.5f, cy - 1, 4, ry, Tone::Ink, 2);
      rect(cx - 7, cy - 2, 15, 2, Tone::Ink);
      break;
    }
    case EyeKind::Flat:  // -_-
      rect(cx - 7, cy - 1, 14, 2, Tone::Ink);
      ellipse(cx, cy + 1, 3, 2.5f, Tone::Ink, 2);
      break;
    case EyeKind::Shades: {  // a dark lens with a white glint
      paint(cx - 9, cy - 5, cx + 9, cy + 6, [&](float px, float py) {
        return inEllipse(px, py, cx, cy, 9, 6.5f) && py >= cy - 4.5f ? (int)Tone::Ink : -1;
      });
      line(cx - 5, cy + 1, cx - 2, cy - 2.5f, 1.3f, Tone::Glint);
      break;
    }
    default:
      break;
  }
}

// ------------------------------------------------------------------ mouth
void FacePainter::drawMouth(float cx, float cy, const FacePose& p) {
  switch (p.mouth) {
    case MouthKind::Grin:  // BMO's own open mouth (proportions from the show's face)
      mouth(cx, cy, 19, 5.5f, 9.5f, 3.2f, 10, 6);
      break;
    case MouthKind::BigGrin:
      mouth(cx, cy, 22, 6.5f, 12, 3.5f, 12, 7);
      break;
    case MouthKind::BigO:
      mouth(cx, cy + 1, 8, 7, 8, 2, 4.5f, 4);
      break;
    case MouthKind::Tongue:  // playful: tongue out over the lower lip
      mouth(cx, cy, 16, 6.5f, 7, 3, 0, 0);
      ellipse(cx + 5, cy + 7.5f, 5, 5, Tone::Lip, 2);
      ellipse(cx + 5, cy + 7.5f, 3.6f, 3.6f, Tone::Tongue, 2);
      break;
    case MouthKind::Smile:
      ring(cx, cy - 3, 11, 3.5f, 2, Tone::Ink, 2);
      break;
    case MouthKind::Flat:
      rect(cx - 8, cy, 16, 2, Tone::Ink);
      break;
    case MouthKind::Frown:
      ring(cx, cy + 4, 10, 3.5f, 2, Tone::Ink, 1);
      break;
    case MouthKind::Wavy: {
      float px = cx - 10, py = cy + 1.5f * sinf(-10 * 0.6f);
      for (int x = -9; x <= 10; x++) {
        float y = cy + 1.5f * sinf(x * 0.6f);
        line(px, py + 0.5f, cx + x, y + 0.5f, 2, Tone::Ink);
        px = cx + x;
        py = y;
      }
      break;
    }
    case MouthKind::SmallO:
      ellipse(cx, cy + 0.5f, 4, 4.5f, Tone::Lip);
      ellipse(cx, cy + 0.5f, 2, 2.5f, Tone::Hollow);
      break;
    case MouthKind::Smirk:
      line(cx - 9, cy + 1, cx + 2, cy + 1, 2, Tone::Ink);
      line(cx + 2, cy + 1, cx + 6, cy, 2, Tone::Ink);
      line(cx + 6, cy, cx + 10, cy - 3, 2, Tone::Ink);
      break;
    case MouthKind::Zipper:
      rect(cx - 11, cy, 23, 2, Tone::Ink);
      for (int x = -10; x <= 10; x += 4) rect(cx + x, cy - 2, 1, 6, Tone::Ink);
      break;
    case MouthKind::Talk: {  // closed -> small -> medium -> BMO's full open mouth
      int step = (int)lroundf(p.mouthOpen * 3);
      if (step <= 0) ring(cx, cy - 3, 11, 3.5f, 2, Tone::Ink, 2);
      else if (step == 1) mouth(cx, cy, 12, 4, 4.5f, 2, 0, 0);
      else if (step == 2) mouth(cx, cy, 16, 5, 7, 2.5f, 7, 4);
      else mouth(cx, cy, 19, 5.5f, 9.5f, 3.2f, 10, 6);
      break;
    }
    case MouthKind::Wail:  // crying: tall, round on top, teeth showing, tongue below
      mouth(cx, cy + 4, 12, 9, 2.5f, 2, 6, 2.5f);
      break;
    case MouthKind::Pout:
      ring(cx, cy + 3, 6, 3, 2, Tone::Ink, 1);
      break;
    case MouthKind::ShySmile:
      ring(cx, cy - 1.5f, 5, 2.5f, 2, Tone::Ink, 2);
      break;
    case MouthKind::Grit: {  // clenched teeth
      paint(cx - 11, cy - 4, cx + 11, cy + 5, [&](float px, float py) {
        bool body = inEllipse(px, py, cx, cy, 11, 5) && py > cy - 3.5f && py < cy + 4;
        if (!body) return -1;
        bool edge = !(inEllipse(px, py, cx, cy, 9.6f, 3.8f) && py > cy - 2.3f && py < cy + 2.8f);
        bool gap = fabsf(py - cy) < 0.5f || fmodf(fabsf(px - cx) + 0.5f, 4.0f) < 1.0f;
        return edge || gap ? (int)Tone::Lip : (int)Tone::Teeth;
      });
      break;
    }
    default:
      break;
  }
}

// ------------------------------------------------------------------ marks
void FacePainter::marks(const FacePose& p, const FaceLayout& l, float dx, float dy) {
  const float ex0 = l.eyeX[0] + dx, ex1 = l.eyeX[1] + dx, ey = l.eyeY + dy;
  const float mx = l.mouthX + dx, my = l.mouthY + dy;
  uint32_t t = p.phase;
  uint32_t m = p.marks;
  if (m & kMarkBlush) {
    ellipse(ex0 - 6, ey + 12, 6, 2.2f, Tone::Blush);
    ellipse(ex1 + 6, ey + 12, 6, 2.2f, Tone::Blush);
  }
  if (m & kMarkShyLines) {
    for (int i = 0; i < 3; i++) {
      line(ex0 - 10 + i * 3, ey + 14, ex0 - 8 + i * 3, ey + 10, 1.2f, Tone::Ink);
      line(ex1 + 4 + i * 3, ey + 14, ex1 + 6 + i * 3, ey + 10, 1.2f, Tone::Ink);
    }
  }
  if (m & kMarkBrows) {
    line(ex0 - 7, ey - 9, ex0 + 5, ey - 14, 2, Tone::Ink);
    line(ex1 - 5, ey - 14, ex1 + 7, ey - 9, 2, Tone::Ink);
  }
  if (m & kMarkTear) {
    float y = ey + 9 + (float)((t / 140) % 7);
    ellipse(ex0 - 8, y + 1.5f, 2.2f, 2.2f, Tone::Tear);
    float tri[6] = {ex0 - 10, y + 1, ex0 - 6, y + 1, ex0 - 8, y - 3};
    polygon(tri, 3, Tone::Tear);
  }
  if (m & kMarkStreams) {  // tears running down from both eyes, drops falling
    for (int i = 0; i < 2; i++) {
      float x = i ? ex1 + 3 : ex0 - 5;
      rect(x, ey + 3, 2.5f, 13, Tone::Tear);
      float y = ey + 6 + (float)(((t / 60) + i * 7) % 14);
      ellipse(x + 1.25f, y, 2, 2, Tone::Tear);
    }
  }
  if (m & kMarkJoyTears) {
    for (int i = 0; i < 2; i++) {
      float x = i ? ex1 + 10 : ex0 - 10, y = ey - 1 + (float)(((t / 90) + i * 3) % 5);
      ellipse(x, y + 1.5f, 2.3f, 2.3f, Tone::Tear);
      float tri[6] = {x - 2.2f, y + 1, x + 2.2f, y + 1, x, y - 3};
      polygon(tri, 3, Tone::Tear);
    }
  }
  if (m & kMarkSweat) {
    float x = ex1 + 17, y = ey - 12;
    ring(x, y + 4, 3, 3, 1.2f, Tone::Ink, 2);
    line(x - 3, y + 4, x, y - 2, 1.2f, Tone::Ink);
    line(x + 3, y + 4, x, y - 2, 1.2f, Tone::Ink);
  }
  if (m & kMarkQuestion) {
    float x = ex1 + 15, y = ey - 1;
    const float pts[][2] = {{0, -9}, {1, -11}, {3, -12}, {5, -11}, {6, -9}, {5, -7}, {3, -6}, {3, -4}};
    for (int i = 0; i + 1 < 8; i++) line(x + pts[i][0], y + pts[i][1], x + pts[i + 1][0], y + pts[i + 1][1], 2, Tone::Ink);
    ellipse(x + 3, y - 0.5f, 1.3f, 1.3f, Tone::Ink);
  }
  if (m & kMarkExclaim) {
    float x = ex1 + 17, y = ey - 1;
    line(x, y - 12, x, y - 5, 2.2f, Tone::Ink);
    ellipse(x, y - 0.5f, 1.4f, 1.4f, Tone::Ink);
  }
  if (m & kMarkSparkle) {
    const float pts[2][2] = {{ex0 - 21, ey - 13}, {ex1 + 21, ey - 15}};
    for (int i = 0; i < 2; i++) {
      float len = 1.5f + (float)(((t / 160) + i * 2) % 3);  // twinkle
      line(pts[i][0] - len, pts[i][1], pts[i][0] + len, pts[i][1], 1.2f, Tone::Ink);
      line(pts[i][0], pts[i][1] - len, pts[i][0], pts[i][1] + len, 1.2f, Tone::Ink);
    }
  }
  if (m & kMarkThinkDots) {
    int n = (int)((t / 350) % 4);  // bubbles appear one by one
    const float b[3][3] = {{ex1 + 12, ey - 9, 1.2f}, {ex1 + 18, ey - 14, 2}, {ex1 + 26, ey - 19, 3}};
    for (int i = 0; i < n; i++) ring(b[i][0], b[i][1], b[i][2], b[i][2], 1.1f, Tone::Ink);
  }
  if (m & kMarkSoundWaves) {
    int n = p.level > 0 ? 1 + p.level / 34 : 1 + (int)((t / 600) % 2);
    if (n > 3) n = 3;
    for (int i = 0; i < n; i++) {
      float r = 4 + 4 * i;
      ring(ex0 - 20, ey + 4, r, r + 2, 1.1f, Tone::Ink, 3);
      ring(ex1 + 20, ey + 4, r, r + 2, 1.1f, Tone::Ink, 4);
    }
  }
  if (m & kMarkZzz) {
    for (int i = 0; i < 3; i++) {
      float ph = (float)((t + i * 900) % 2700) / 2700.0f;
      if (ph > 0.92f) continue;
      glyphZ(ex1 + 14 + ph * 14, ey + 2 - ph * 20, 3 + ph * 5);
    }
  }
  if (m & kMarkVein) {  // the "hmph!" anger mark, gently pulsing
    float x = ex1 + 18, y = ey - 13, s = (t % 600) < 300 ? 1.0f : 1.25f;
    const float arms[4][6] = {{-3, -1, -1, -1, -1, -3}, {1, -3, 1, -1, 3, -1}, {3, 1, 1, 1, 1, 3}, {-1, 3, -1, 1, -3, 1}};
    for (auto& a : arms) {
      line(x + a[0] * s, y + a[1] * s, x + a[2] * s, y + a[3] * s, 1.4f, Tone::Ink);
      line(x + a[2] * s, y + a[3] * s, x + a[4] * s, y + a[5] * s, 1.4f, Tone::Ink);
    }
  }
  if (m & kMarkHearts) {  // little hearts floating up
    for (int i = 0; i < 2; i++) {
      float ph = (float)((t + i * 1100) % 2200) / 2200.0f;
      heart(ex1 + 17 + 4 * sinf(ph * 6.28f), ey + 4 - ph * 22, 6 + 2 * ph, Tone::Heart);
    }
  }
  if (m & kMarkOrbit) {  // little stars circling above the head
    for (int i = 0; i < 3; i++) {
      float a = (float)(t % 1800) / 1800.0f * 2 * kPi + i * 2 * kPi / 3;
      star(64 + dx + 22 * cosf(a), 6 + 3 * sinf(a), 2.8f, 0, Tone::Star);
    }
  }
  if (m & kMarkSigh) {  // a little puff drifting away from the mouth
    float ph = (float)(t % 1600) / 1600.0f;
    ring(mx - 16 - ph * 6, my - 2 - ph * 4, 2 + ph, 1.5f + ph, 1.1f, Tone::Ink);
    ring(mx - 22 - ph * 6, my - 5 - ph * 4, 1.2f + ph, 1 + ph, 1.0f, Tone::Ink);
  }
  if (m & kMarkWifiOff) wifiOffIcon(3, 2);
  if (m & kMarkMicOff) micOffIcon(114, 1);
}

// ------------------------------------------------------------------ icons
void FacePainter::micOffIcon(float x, float y) {
  ellipse(x + 5.5f, y + 4, 2.5f, 4, Tone::Ink);
  ring(x + 5.5f, y + 5, 5, 4, 1.1f, Tone::Ink, 2);
  rect(x + 5, y + 10, 1, 2, Tone::Ink);
  rect(x + 2, y + 12, 7, 1, Tone::Ink);
  line(x, y + 13.5f, x + 10.5f, y, 3.2f, Tone::Screen);  // keeps the slash readable
  line(x, y + 13.5f, x + 10.5f, y, 1.5f, Tone::Ink);
}

void FacePainter::wifiOffIcon(float x, float y) {
  ring(x + 5.5f, y + 8.5f, 7, 7, 1.1f, Tone::Ink, 1);
  ring(x + 5.5f, y + 8.5f, 4, 4, 1.1f, Tone::Ink, 1);
  ellipse(x + 5.5f, y + 8.5f, 1.3f, 1.3f, Tone::Ink);
  line(x, y, x + 10.5f, y + 9.5f, 1.2f, Tone::Ink);
}

// ------------------------------------------------------------------ face
void FacePainter::draw(const FacePose& p, const FaceLayout& l) {
  float dx = p.shake, dy = p.bob;
  for (int i = 0; i < 2; i++)
    eye(l.eyeX[i] + p.eye[i].dx + dx, l.eyeY + p.eye[i].dy + dy, p.eye[i], i == 0, p.phase);
  if (p.eye[0].kind == EyeKind::Shades)  // the bridge of the sunglasses
    rect(l.eyeX[0] + 8 + dx, l.eyeY - 3.5f + dy, l.eyeX[1] - l.eyeX[0] - 16, 2, Tone::Ink);
  drawMouth(l.mouthX + p.mouthDx + dx, l.mouthY + dy, p);
  marks(p, l, dx, dy);
}

}  // namespace robot
