// Face canvas: HOW a FacePose (face_model.h) is drawn — once, for every kind of display.
//
// The painter works in the 128x64 "face space" with sub-pixel geometry (ellipses, strokes,
// polygons tested at pixel centres) and writes TONES, not colours, to a Surface:
//   * the SH1106 (U8g2 surface) maps tones to lit / dark / half-tone pixels,
//   * a colour display or the colour previews map them to BMO's own palette (bmoColor),
// scaled by any factor (1 = SH1106 128x64, 2.5 = a 320x170 colour TFT). Portable: no U8g2,
// so the exact drawing is unit-tested on the host.
#pragma once
#include <stdint.h>

#include "face_model.h"

namespace robot {

enum class Tone : uint8_t {
  Screen,  // BMO's mint-green screen
  Ink,     // eyes, lines, marks
  Lip,     // dark-green edge of the open mouth
  Mouth,   // green inside of the mouth
  Teeth,   // white
  Tongue,  // light green
  Blush,   // pink cheeks
  Heart,   // red-pink (love eyes, little hearts)
  Star,    // gold (starstruck eyes)
  Tear,    // light blue
  Glint,   // white highlight (sunglasses)
  Hollow,  // inside of a small "o" mouth
  kCount
};

class Surface {
 public:
  virtual ~Surface() = default;
  virtual int width() const = 0;
  virtual int height() const = 0;
  virtual void plot(int x, int y, Tone t) = 0;  // called only with 0 <= x < width, 0 <= y < height
};

struct Rgb {
  uint8_t r, g, b;
};
Rgb bmoColor(Tone t);                       // colours sampled from BMO's face
inline uint16_t toRgb565(Rgb c) { return (uint16_t)(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3)); }
uint8_t monoValue(Tone t, int x, int y);    // 1 = ink pixel, 0 = screen pixel (before the light-screen invert)

// A colour frame buffer (3 bytes per pixel), e.g. for previews or a TFT driver.
class RgbSurface : public Surface {
 public:
  RgbSurface(uint8_t* rgb, int w, int h) : buf_(rgb), w_(w), h_(h) {}
  int width() const override { return w_; }
  int height() const override { return h_; }
  void plot(int x, int y, Tone t) override;
  void clear(Tone t = Tone::Screen);
  void setDim(bool dim) { dim_ = dim; }  // sleeping: the screen dims

 private:
  uint8_t* buf_;
  int w_, h_;
  bool dim_ = false;
};

// Proportions measured from BMO's face: round dot eyes at ~27 % / 73 % of the width and 40 % of
// the height; a wide open mouth (~30 % of the width) whose corners sit at ~66 % of the height.
struct FaceLayout {
  float eyeX[2] = {34, 94};
  float eyeY = 25;
  float mouthX = 64;  // the mouth is symmetric around this line (a pixel boundary)
  float mouthY = 42;  // the line through the mouth's corners
};

class FacePainter {
 public:
  explicit FacePainter(Surface& s, float scale = 1.0f) : s_(s), k_(scale) {}

  void draw(const FacePose& p, const FaceLayout& l = FaceLayout());
  void micOffIcon(float x, float y);   // 11 x 14
  void wifiOffIcon(float x, float y);  // 11 x 9

  // Primitives, in face-space units. part: 0 all, 1 upper, 2 lower, 3 left, 4 right half.
  void ellipse(float cx, float cy, float rx, float ry, Tone t, int part = 0);
  void ring(float cx, float cy, float rx, float ry, float w, Tone t, int part = 0);
  void line(float x0, float y0, float x1, float y1, float w, Tone t);
  void rect(float x, float y, float w, float h, Tone t);
  void polygon(const float* xy, int n, Tone t);
  // BMO's open mouth: dark-green lip, green inside, white teeth under the top lip, light-green
  // tongue rising from the bottom. top/bottom: depth above/below the corner line.
  void mouth(float cx, float cy, float rx, float top, float bottom, float teeth, float tongueRx, float tongueRy);

 private:
  template <class F>
  void paint(float x0, float y0, float x1, float y1, F toneAt);
  void eye(float cx, float cy, const EyePose& e, bool left, uint32_t phase);
  void drawMouth(float cx, float cy, const FacePose& p);
  void marks(const FacePose& p, const FaceLayout& l, float dx, float dy);
  void heart(float cx, float cy, float size, Tone t);
  void star(float cx, float cy, float r, float turn, Tone t);
  void glyphZ(float x, float y, float h);

  Surface& s_;
  float k_;
};

}  // namespace robot
