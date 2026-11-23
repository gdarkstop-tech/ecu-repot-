#include "face_model.h"

#include <math.h>

namespace robot {

namespace {

constexpr float kTwoPi = 6.2831853f;

inline bool reached(uint32_t now, uint32_t when) { return (int32_t)(now - when) >= 0; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float smoothstep(float x) {
  x = clampf(x, 0, 1);
  return x * x * (3 - 2 * x);
}

// Slow, small "looking around" pattern (px). A fixed cycle, not random: calm and repeatable.
const int8_t kGaze[][2] = {{0, 0}, {2, 0}, {0, 0}, {-2, -1}, {0, 0}, {1, 1}, {0, 0}, {-1, 0}};
constexpr int kGazeSteps = sizeof(kGaze) / sizeof(kGaze[0]);

bool canBlink(EyeKind k) { return k == EyeKind::Dot || k == EyeKind::Lidded || k == EyeKind::Sleepy; }

bool drifts(Expression e) {
  return e == Expression::Idle || e == Expression::Blink || e == Expression::Listening ||
         e == Expression::Offline || e == Expression::Muted || e == Expression::Curious ||
         e == Expression::Happy || e == Expression::Sleepy;
}

}  // namespace

// ------------------------------------------------------------------ the designs
FacePose designFor(Expression e) {
  FacePose p;
  auto eyes = [&](EyeKind l, EyeKind r) {
    p.eye[0].kind = l;
    p.eye[1].kind = r;
  };
  switch (e) {
    case Expression::Idle:
    case Expression::Blink:
    case Expression::Boot:
      p.mouth = MouthKind::Smile;
      break;
    case Expression::Happy:
      p.mouth = MouthKind::Grin;
      break;
    case Expression::Excited:
      eyes(EyeKind::Arc, EyeKind::Arc);
      p.mouth = MouthKind::BigGrin;
      p.marks = kMarkBlush | kMarkSparkle;
      break;
    case Expression::Listening:
      p.eye[0].scale = p.eye[1].scale = 1.15f;
      p.eye[0].dy = p.eye[1].dy = -1;
      p.mouth = MouthKind::Smile;
      p.marks = kMarkSoundWaves;
      break;
    case Expression::Thinking:
      p.eye[0].dx = p.eye[1].dx = -3;
      p.eye[0].dy = p.eye[1].dy = -3;
      p.mouth = MouthKind::Flat;
      p.mouthDx = 6;
      p.marks = kMarkThinkDots;
      break;
    case Expression::Speaking:
      p.mouth = MouthKind::Talk;
      break;
    case Expression::Sleeping:
      eyes(EyeKind::Asleep, EyeKind::Asleep);
      p.mouth = MouthKind::SmallO;
      p.marks = kMarkZzz;
      p.darkScreen = true;
      break;
    case Expression::Sleepy:
      eyes(EyeKind::Sleepy, EyeKind::Sleepy);
      p.eye[0].dy = p.eye[1].dy = 1;
      p.mouth = MouthKind::SmallO;  // a little yawn
      break;
    case Expression::Surprised:
      p.eye[0].scale = p.eye[1].scale = 1.35f;
      p.eye[0].dy = p.eye[1].dy = -1;
      p.mouth = MouthKind::BigO;
      p.marks = kMarkExclaim;
      break;
    case Expression::Sad:
      p.eye[0].dy = p.eye[1].dy = 2;
      p.mouth = MouthKind::Frown;
      p.marks = kMarkBrows | kMarkTear;
      break;
    case Expression::Worried:
      p.mouth = MouthKind::Wavy;
      p.marks = kMarkBrows | kMarkSweat;
      break;
    case Expression::Curious:
      p.eye[0].scale = 1.25f;
      p.eye[1].scale = 0.9f;
      p.eye[1].dy = -2;
      p.mouth = MouthKind::SmallO;
      p.mouthDx = 4;
      break;
    case Expression::Confused:
      eyes(EyeKind::Dot, EyeKind::Squint);
      p.mouth = MouthKind::Wavy;
      p.marks = kMarkQuestion;
      break;
    case Expression::Playful:
      eyes(EyeKind::Arc, EyeKind::Dot);  // a wink
      p.mouth = MouthKind::Tongue;
      p.marks = kMarkBlush;
      break;
    case Expression::Proud:
      eyes(EyeKind::Lidded, EyeKind::Lidded);
      p.mouth = MouthKind::Smirk;
      p.marks = kMarkBlush | kMarkSparkle;
      break;
    case Expression::Muted:
      p.mouth = MouthKind::Zipper;
      p.marks = kMarkMicOff;
      break;
    case Expression::Offline:
      p.eye[0].dy = p.eye[1].dy = 2;
      p.mouth = MouthKind::Flat;
      p.marks = kMarkWifiOff;
      break;
    case Expression::Error:
      eyes(EyeKind::Cross, EyeKind::Cross);
      p.mouth = MouthKind::Wavy;
      break;
    default:
      break;
  }
  return p;
}

// ------------------------------------------------------------------ animation
void FaceAnimator::reset(uint32_t now, uint32_t seed) {
  *this = FaceAnimator();
  rng_ = seed ? seed : 1;
  init_ = true;
  last_ = now;
  nextBlink_ = now + 2600;
  nextGaze_ = now + 2400;
}

uint32_t FaceAnimator::rand32() {
  // xorshift32: deterministic, so the simulator's images and the tests are reproducible.
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return rng_;
}

void FaceAnimator::startBlink(uint32_t now, bool twice) {
  blinkActive_ = true;
  blinkTwice_ = twice;
  blinkStart_ = now;
  blinkCount_++;
}

// One blink: close 70 ms, stay closed 40 ms, open 90 ms (a sleepy face blinks twice as slowly).
float FaceAnimator::blinkOpen(uint32_t now) const {
  if (!blinkActive_) return 1.0f;
  float k = shownExpr_ == Expression::Sleepy ? 2.0f : 1.0f;
  float close = 70 * k, hold = 40 * k, open = 90 * k, cycle = close + hold + open;
  float t = (float)(now - blinkStart_);
  if (blinkTwice_ && t >= cycle + 60) t -= cycle + 60;
  if (t < close) return 1.0f - t / close;
  if (t < close + hold) return 0.0f;
  if (t < cycle) return (t - close - hold) / open;
  return 1.0f;
}

bool FaceAnimator::blinking(uint32_t now) const { return blinkActive_ && blinkOpen(now) < 0.999f; }

float FaceAnimator::speechLevel(const ViewModel& vm, uint32_t now) {
  if (vm.audioLevel > 0) return vm.audioLevel / 100.0f;  // real playback envelope
  // No level available (e.g. the "say" command): a calm, deterministic talking rhythm.
  float t = now / 1000.0f;
  float env = 0.5f + 0.5f * sinf(t * 9.0f) * sinf(t * 2.3f + 1.0f);
  float gate = sinf(t * 3.1f) > -0.6f ? 1.0f : 0.15f;
  return env * gate;
}

FacePose FaceAnimator::update(const ViewModel& vm, uint32_t now) {
  if (!init_) reset(now);
  float dt = clampf((float)(int32_t)(now - last_), 0.0f, 100.0f);
  last_ = now;

  Expression e = vm.expression;
  if (vm.blinkSeq != lastBlinkSeq_) {  // explicit blink (button, debug command, selftest)
    lastBlinkSeq_ = vm.blinkSeq;
    startBlink(now, false);
  }
  if (e != expr_) {
    // Change expressions behind a blink: the new face appears while the eyes are closed.
    bool wasBoot = expr_ == Expression::Boot;
    expr_ = e;
    if (wasBoot) shownExpr_ = e;
    else if (!blinkActive_) startBlink(now, false);
  }
  float open = blinkOpen(now);
  if (shownExpr_ != expr_ && (!blinkActive_ || open < 0.35f)) shownExpr_ = expr_;
  float cycle = shownExpr_ == Expression::Sleepy ? 400.0f : 200.0f;
  if (blinkActive_ && (float)(now - blinkStart_) >= (blinkTwice_ ? 2 * cycle + 60 : cycle)) {
    blinkActive_ = false;
    open = 1.0f;
  }

  FacePose p = designFor(shownExpr_);

  // Natural blinking: every 2.8–5.2 s, sometimes twice.
  if (!blinkActive_ && reached(now, nextBlink_)) {
    if (canBlink(p.eye[0].kind) || canBlink(p.eye[1].kind)) startBlink(now, rand32() % 6 == 0);
    nextBlink_ = now + 2800 + rand32() % 2400;
  }
  open = blinkOpen(now);

  // Gaze drift: a small fixed pattern, eased (no saccade jumps).
  if (reached(now, nextGaze_)) {
    gazeStep_ = (uint8_t)((gazeStep_ + 1) % kGazeSteps);
    gazeFromX_ = gazeToX_;
    gazeFromY_ = gazeToY_;
    gazeToX_ = kGaze[gazeStep_][0];
    gazeToY_ = kGaze[gazeStep_][1];
    gazeStart_ = now;
    nextGaze_ = now + 2600 + rand32() % 1600;
  }
  float g = smoothstep((float)(now - gazeStart_) / 320.0f);
  float gx = gazeFromX_ + (gazeToX_ - gazeFromX_) * g;
  float gy = gazeFromY_ + (gazeToY_ - gazeFromY_) * g;
  if (!drifts(shownExpr_)) gx = gy = 0;
  if (shownExpr_ == Expression::Curious) gx *= 1.5f;

  float a = dt / (90.0f + dt);  // ~90 ms smoothing: posture changes glide, never jump
  for (int i = 0; i < 2; i++) {
    eyeDx_[i] += (p.eye[i].dx + gx - eyeDx_[i]) * a;
    eyeDy_[i] += (p.eye[i].dy + gy - eyeDy_[i]) * a;
    eyeScale_[i] += (p.eye[i].scale - eyeScale_[i]) * a;
    p.eye[i].dx = eyeDx_[i];
    p.eye[i].dy = eyeDy_[i];
    p.eye[i].scale = eyeScale_[i];
    p.eye[i].open = canBlink(p.eye[i].kind) ? open : 1.0f;
  }

  // Speaking mouth: smoothed level, 4 shapes, each held >= 70 ms (no flicker).
  if (p.mouth == MouthKind::Talk) {
    float raw = speechLevel(vm, now);
    float k = raw > level_ ? dt / (25.0f + dt) : dt / (90.0f + dt);  // quick attack, gentle release
    level_ += (raw - level_) * k;
    int want = level_ < 0.12f ? 0 : (level_ < 0.38f ? 1 : (level_ < 0.65f ? 2 : 3));
    if (want != mouthStep_ && (uint32_t)(now - mouthHeldSince_) >= 70) {
      mouthStep_ = (uint8_t)(want > mouthStep_ ? mouthStep_ + 1 : mouthStep_ - 1);  // one step at a time
      mouthHeldSince_ = now;
    }
    p.mouthOpen = mouthStep_ / 3.0f;
  } else {
    level_ = 0;
    mouthStep_ = 0;
  }

  if (shownExpr_ == Expression::Excited) p.bob = sinf(kTwoPi * (float)(now % 700) / 700.0f);
  if (shownExpr_ == Expression::Sleeping) p.bob = 0.8f * sinf(kTwoPi * (float)(now % 4000) / 4000.0f);
  p.level = vm.micActive ? vm.audioLevel : 0;
  p.phase = now;
  return p;
}

}  // namespace robot
