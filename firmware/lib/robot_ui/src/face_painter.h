// Monochrome painter for the face model (face_model.h) on a 128x64 U8g2 buffer.
//
// Draws in "ink" terms: Role::Feature = pixel value 1, Role::Screen = 0, Role::Accent = a 50 %
// dither (reads as a third tone on the OLED, used for blush and tongue). FaceRenderer then
// inverts the whole buffer for the light, BMO-like screen (dark features on a lit screen).
// A colour display would get its own painter that maps the same roles to colours.
#pragma once
#include <stdint.h>
#include <clib/u8g2.h>

#include "face_model.h"

namespace robot {

// Proportions measured from BMO's face: round dot eyes at ~27 % / 73 % of the width and 40 % of
// the height; a wide open mouth (~30 % of the width) whose corners sit at ~66 % of the height.
struct FaceLayout {
  int eyeX[2] = {34, 94};
  int eyeY = 25;
  int mouthX = 64;  // the mouth is symmetric around the pixel boundary 63|64
  int mouthY = 42;  // the line through the mouth's corners
};

class MonoFacePainter {
 public:
  void draw(u8g2_t* g, const FacePose& p, const FaceLayout& l = FaceLayout());

  // Primitives in role terms (also used by FaceRenderer for icons and the boot animation).
  static void setRole(u8g2_t* g, Role r);
  static void fillEllipse(u8g2_t* g, int cx, int cy, int rx, int ry, Role r, uint8_t opt = U8G2_DRAW_ALL);
  static void thickHLine(u8g2_t* g, int x, int y, int w, int t = 2);
  static void thickLine(u8g2_t* g, int x0, int y0, int x1, int y1);
  static void arc(u8g2_t* g, int cx, int cy, int rx, int ry, uint8_t opt);  // 2 px outline
  // Sub-pixel ellipse: fills pixels whose centres lie inside. part: 0 all, 1 upper, 2 lower half.
  static void fillEllipseF(u8g2_t* g, float cx, float cy, float rx, float ry, Role r, int part = 0);
  // BMO's open mouth: dark body (flat-ish top, deeper round bottom), a lit teeth band under the
  // top lip and a lighter (dithered) tongue at the bottom.
  static void bmoMouth(u8g2_t* g, float cx, float cy, float rx, float top, float bottom, int teeth,
                       float tongueRx, float tongueRy);
  static void micOffIcon(u8g2_t* g, int x, int y);   // 11 x 14
  static void wifiOffIcon(u8g2_t* g, int x, int y);  // 11 x 9

 private:
  void eye(u8g2_t* g, int cx, int cy, const EyePose& e, bool left);
  void mouth(u8g2_t* g, int cx, int cy, const FacePose& p);
  void marks(u8g2_t* g, const FacePose& p, const FaceLayout& l, int bob);
};

}  // namespace robot
