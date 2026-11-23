// OLED screen renderer for a 128x64 monochrome display (SH1106), BMO-inspired.
//
// Uses only the U8g2 C API, so the exact same drawing code runs on the ESP32-S3 and in the
// host simulator (which renders reference images for review and tests).
//
// The face itself is described by the display-independent face model (face_model.h: poses +
// animation) and drawn by MonoFacePainter. By default the screen is "light": the buffer is
// inverted at the end of each frame so the OLED shows dark features on a lit screen, like BMO.
// Sleeping switches to a dark screen (and saves the OLED). What to show is decided by Robot and
// delivered as a ViewModel each frame.
#pragma once
#include <stdint.h>
#include <clib/u8g2.h>

#include "face_model.h"
#include "face_painter.h"
#include "view_model.h"

namespace robot {

class FaceRenderer {
 public:
  static constexpr int kWidth = 128;
  static constexpr int kHeight = 64;

  void reset(uint32_t now, uint32_t seed = 0x2545F491u);
  // true (default): lit screen, dark features (BMO). false: dark screen, lit features.
  void setLightScreen(bool on) { light_ = on; }
  bool lightScreen() const { return light_; }

  // Draws one complete frame into the U8g2 buffer. The caller sends it to the display.
  void render(u8g2_t* g, const ViewModel& vm, uint32_t now);
  const FacePose& pose() const { return pose_; }

 private:
  void drawFace(u8g2_t* g, const ViewModel& vm, uint32_t now);
  void drawBoot(u8g2_t* g, const ViewModel& vm, uint32_t now);
  void drawStatusIcons(u8g2_t* g, const ViewModel& vm);
  void drawLabel(u8g2_t* g, const char* text);
  void finish(u8g2_t* g, bool darkScreen);

  // screens
  void drawContent(u8g2_t* g, const ViewModel& vm, uint32_t now);
  void drawOverlayText(u8g2_t* g, const char* text, bool rtl);
  void drawReading(u8g2_t* g, const ViewModel& vm, uint32_t now);
  void drawMarquee(u8g2_t* g, const ViewModel& vm, uint32_t now, int top);

  FaceAnimator anim_;
  MonoFacePainter painter_;
  FaceLayout layout_;
  FacePose pose_;
  bool light_ = true;
  bool initialized_ = false;
};

// Draws UTF-8 text enlarged by the largest integer factor (<= maxScale) whose INK box fits
// maxW x maxH, centred at centerX within [top, top+maxH]. Used for big Arabic glyphs because
// U8g2's Arabic font only exists at 16 px. Must be called before anything else is drawn in the
// top-left 128x24 area of the buffer (it uses it as scratch). Returns the scale used (0 = nothing).
int drawFittedUtf8(u8g2_t* g, const uint8_t* font, const char* text, int centerX, int top, int maxW, int maxH,
                   int maxScale);

}  // namespace robot
