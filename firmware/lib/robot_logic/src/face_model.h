// Face model: WHAT the face looks like, independent of the display.
//
// The robot is styled after BMO (Adventure Time): a light "screen" with small dark dot eyes and
// a simple mouth. Each Expression is described as a FacePose — eye shapes, mouth shape, small
// marks (blush, sweat drop, "?", Zz...) and offsets — and FaceAnimator turns the current
// ViewModel into one pose per frame: soft periodic blinks, slow deterministic gaze drift, a
// mouth that follows the speech level, a blink on every expression change. No randomness that
// could look jittery: everything is a pure function of a seeded schedule and time.
//
// The painter (face_canvas.h) draws the pose on any Surface with semantic tones (screen, ink,
// lip, mouth, teeth, tongue, blush...): the SH1106 maps them to lit / dark / dithered pixels,
// a colour display (and the colour previews) to BMO's own palette — the same drawing code.
#pragma once
#include <stdint.h>

#include "robot_types.h"
#include "view_model.h"

namespace robot {

enum class EyeKind : uint8_t {
  Dot,      // BMO's oval dot
  Closed,   // a short line (blink)
  Arc,      // ^  happy / excited / wink
  Lidded,   // dot with a flat upper lid (proud, smug)
  Sleepy,   // lower half only, heavy lid (sleepy)
  Asleep,   // relaxed downward curve (sleeping)
  Cross,    // x  error
  Squint,   // thin raised line (one confused eye)
  Heart,    // love
  Star,     // starstruck
  Spiral,   // dizzy (turns)
  Squeeze,  // > <  laughing hard
  Grumpy,   // dot under a lid slanting down to the nose: annoyed
  SideEye,  // half-lidded, looking sideways: mischievous
  Flat,     // -_-  bored
  Shades,   // sunglasses lens: cool
  kCount
};

enum class MouthKind : uint8_t {
  Smile,    // small curved line
  Flat,
  Grin,     // open "D" with tongue
  BigGrin,
  Frown,
  Wavy,
  SmallO,
  BigO,
  Smirk,
  Tongue,   // smile with the tongue out
  Zipper,   // muted
  Talk,     // speaking: shape follows FacePose::mouthOpen
  Wail,     // crying: upside-down open mouth
  Pout,     // annoyed: small tight frown
  ShySmile, // a tiny smile
  Grit,     // scared: clenched teeth
  kCount
};

// Small marks around the face (bit flags).
enum Mark : uint32_t {
  kMarkBlush = 1 << 0,
  kMarkSweat = 1 << 1,
  kMarkQuestion = 1 << 2,
  kMarkSparkle = 1 << 3,
  kMarkZzz = 1 << 4,
  kMarkThinkDots = 1 << 5,
  kMarkSoundWaves = 1 << 6,
  kMarkTear = 1 << 7,
  kMarkBrows = 1 << 8,      // worried / sad brows (inner ends raised)
  kMarkExclaim = 1 << 9,
  kMarkWifiOff = 1 << 10,
  kMarkMicOff = 1 << 11,
  kMarkVein = 1 << 12,      // anger mark (annoyed)
  kMarkHearts = 1 << 13,    // little hearts floating up (love)
  kMarkOrbit = 1 << 14,     // stars circling the head (dizzy)
  kMarkStreams = 1 << 15,   // tear streams (crying)
  kMarkJoyTears = 1 << 16,  // tears of joy (laughing)
  kMarkShyLines = 1 << 17,  // /// on the cheeks (shy)
  kMarkSigh = 1 << 18,      // a puff of air (bored)
};

struct EyePose {
  EyeKind kind = EyeKind::Dot;
  float dx = 0, dy = 0;  // offset from the eye's home position (px)
  float scale = 1.0f;    // 1 = standard dot
  float open = 1.0f;     // 1 = open, 0 = closed (blink)
};

struct FacePose {
  EyePose eye[2];        // [0] = left, [1] = right (as seen by the viewer)
  MouthKind mouth = MouthKind::Smile;
  float mouthOpen = 0;   // 0..1, used by Talk (quantised to 4 steps)
  float mouthDx = 0;
  uint32_t marks = 0;
  float bob = 0;         // whole-face vertical offset (px)
  float shake = 0;       // whole-face horizontal offset (px): a scared tremble
  bool darkScreen = false;  // sleeping: the screen dims (also protects the OLED)
  uint8_t level = 0;     // 0..100 microphone level (sound waves)
  uint32_t phase = 0;    // ms clock for mark animations (Zz rise, dots, sparkles)
};

// The static design of an expression (no animation).
FacePose designFor(Expression e);

class FaceAnimator {
 public:
  void reset(uint32_t now, uint32_t seed = 0x2545F491u);
  FacePose update(const ViewModel& vm, uint32_t now);

  // For tests and the simulator.
  bool blinking(uint32_t now) const;
  uint32_t blinkCount() const { return blinkCount_; }

 private:
  uint32_t rand32();
  void startBlink(uint32_t now, bool twice);
  float blinkOpen(uint32_t now) const;
  float speechLevel(const ViewModel& vm, uint32_t now);

  bool init_ = false;
  uint32_t rng_ = 1;
  uint32_t last_ = 0;
  Expression expr_ = Expression::Boot;
  Expression shownExpr_ = Expression::Boot;  // swapped while the eyes are closed
  // blink
  uint32_t blinkStart_ = 0;
  bool blinkActive_ = false;
  bool blinkTwice_ = false;
  uint32_t nextBlink_ = 0;
  uint32_t lastBlinkSeq_ = 0;
  uint32_t blinkCount_ = 0;
  // gaze drift
  uint8_t gazeStep_ = 0;
  uint32_t nextGaze_ = 0;
  float gazeFromX_ = 0, gazeFromY_ = 0, gazeToX_ = 0, gazeToY_ = 0;
  uint32_t gazeStart_ = 0;
  // smoothed values
  float eyeDx_[2] = {0, 0}, eyeDy_[2] = {0, 0}, eyeScale_[2] = {1, 1};
  float level_ = 0;      // smoothed speech level
  uint8_t mouthStep_ = 0;
  uint32_t mouthHeldSince_ = 0;
};

}  // namespace robot
