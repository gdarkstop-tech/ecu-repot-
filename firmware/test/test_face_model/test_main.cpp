// The BMO-inspired face model: every expression is a distinct design, and the animation is
// soft and stable (periodic blinks, no jitter, a talking mouth that does not flicker).
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "face_canvas.h"
#include "face_model.h"
#include "robot_types.h"

using namespace robot;

void setUp() {}
void tearDown() {}

static bool samePose(const FacePose& a, const FacePose& b) {
  for (int i = 0; i < 2; i++)
    if (a.eye[i].kind != b.eye[i].kind || fabsf(a.eye[i].dx - b.eye[i].dx) > 0.01f ||
        fabsf(a.eye[i].dy - b.eye[i].dy) > 0.01f || fabsf(a.eye[i].scale - b.eye[i].scale) > 0.01f)
      return false;
  return a.mouth == b.mouth && a.marks == b.marks && fabsf(a.mouthDx - b.mouthDx) < 0.01f &&
         a.darkScreen == b.darkScreen;
}

void test_every_expression_has_its_own_design() {
  const Expression all[] = {Expression::Idle,    Expression::Happy,   Expression::Excited,  Expression::Listening,
                            Expression::Thinking, Expression::Speaking, Expression::Sleeping, Expression::Sleepy,
                            Expression::Surprised, Expression::Sad,    Expression::Worried,  Expression::Curious,
                            Expression::Confused, Expression::Playful, Expression::Proud,    Expression::Muted,
                            Expression::Offline,  Expression::Error,   Expression::Love,     Expression::Starstruck,
                            Expression::Dizzy,    Expression::Laughing, Expression::Crying,  Expression::Annoyed,
                            Expression::Shy,      Expression::Scared,  Expression::Mischievous, Expression::Bored,
                            Expression::Cool};
  const int n = sizeof(all) / sizeof(all[0]);
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++) {
      char msg[64];
      snprintf(msg, sizeof(msg), "%s vs %s", toString(all[i]), toString(all[j]));
      TEST_ASSERT_FALSE_MESSAGE(samePose(designFor(all[i]), designFor(all[j])), msg);
    }
  // BMO's signature parts: round dot eyes and the open mouth with teeth and tongue
  TEST_ASSERT_EQUAL((int)EyeKind::Dot, (int)designFor(Expression::Idle).eye[0].kind);
  TEST_ASSERT_EQUAL((int)MouthKind::Grin, (int)designFor(Expression::Idle).mouth);
  TEST_ASSERT_EQUAL((int)EyeKind::Arc, (int)designFor(Expression::Excited).eye[0].kind);
  TEST_ASSERT_TRUE(designFor(Expression::Playful).eye[0].kind != designFor(Expression::Playful).eye[1].kind);  // wink
  TEST_ASSERT_EQUAL((int)MouthKind::Zipper, (int)designFor(Expression::Muted).mouth);
  TEST_ASSERT_TRUE(designFor(Expression::Sleeping).darkScreen);
  TEST_ASSERT_FALSE(designFor(Expression::Idle).darkScreen);
  // the distinctive reactions
  TEST_ASSERT_EQUAL((int)EyeKind::Heart, (int)designFor(Expression::Love).eye[0].kind);
  TEST_ASSERT_EQUAL((int)EyeKind::Star, (int)designFor(Expression::Starstruck).eye[1].kind);
  TEST_ASSERT_EQUAL((int)EyeKind::Spiral, (int)designFor(Expression::Dizzy).eye[0].kind);
  TEST_ASSERT_EQUAL((int)EyeKind::Shades, (int)designFor(Expression::Cool).eye[0].kind);
  TEST_ASSERT_TRUE(designFor(Expression::Annoyed).marks & kMarkVein);
  TEST_ASSERT_TRUE(designFor(Expression::Crying).marks & kMarkStreams);
  TEST_ASSERT_TRUE(designFor(Expression::Shy).marks & kMarkShyLines);
}

void test_expression_names_round_trip() {
  const char* names[] = {"excited",  "curious", "confused", "worried", "playful", "proud",   "offline",
                         "sleepy",   "love",    "starstruck", "dizzy", "laughing", "crying", "annoyed",
                         "shy",      "scared",  "mischievous", "bored", "cool"};
  for (const char* n : names) {
    Expression e;
    TEST_ASSERT_TRUE_MESSAGE(parseExpression(n, e), n);
    TEST_ASSERT_EQUAL_STRING(n, toString(e));
  }
  Expression e;
  TEST_ASSERT_FALSE(parseExpression("boot", e));  // internal only
}

static ViewModel vmFor(Expression e) {
  ViewModel vm;
  vm.state = RobotState::Idle;
  vm.expression = e;
  return vm;
}

void test_blinks_are_soft_and_periodic() {
  FaceAnimator a;
  a.reset(0, 42);
  ViewModel vm = vmFor(Expression::Idle);
  uint32_t lastStart = 0, minGap = 0xFFFFFFFF, seen = a.blinkCount();
  int blinks = 0, closedFrames = 0;
  for (uint32_t t = 0; t < 60000; t += 33) {
    a.update(vm, t);
    if (a.blinkCount() != seen) {  // one blink event (sometimes a quick double blink)
      seen = a.blinkCount();
      if (blinks) minGap = t - lastStart < minGap ? t - lastStart : minGap;
      lastStart = t;
      blinks++;
    }
    if (a.blinking(t)) closedFrames++;
  }
  TEST_ASSERT_TRUE_MESSAGE(blinks >= 10 && blinks <= 24, "about one blink every 3-5 s");
  TEST_ASSERT_TRUE_MESSAGE(minGap >= 2500, "never blinks in rapid succession");
  TEST_ASSERT_TRUE_MESSAGE(closedFrames < 60000 / 33 / 10, "eyes open > 90 % of the time");
}

void test_animation_is_deterministic() {
  FaceAnimator a, b;
  a.reset(0, 7);
  b.reset(0, 7);
  ViewModel vm = vmFor(Expression::Idle);
  for (uint32_t t = 0; t < 20000; t += 33) {
    FacePose pa = a.update(vm, t), pb = b.update(vm, t);
    TEST_ASSERT_EQUAL_FLOAT(pa.eye[0].dx, pb.eye[0].dx);
    TEST_ASSERT_EQUAL_FLOAT(pa.eye[0].open, pb.eye[0].open);
  }
}

void test_eyes_drift_slightly_and_never_jump() {
  FaceAnimator a;
  a.reset(0, 3);
  ViewModel vm = vmFor(Expression::Idle);
  float prev = 0, maxStep = 0, maxAbs = 0;
  for (uint32_t t = 0; t < 40000; t += 33) {
    FacePose p = a.update(vm, t);
    float step = fabsf(p.eye[0].dx - prev);
    if (t > 0 && step > maxStep) maxStep = step;
    if (fabsf(p.eye[0].dx) > maxAbs) maxAbs = fabsf(p.eye[0].dx);
    prev = p.eye[0].dx;
  }
  TEST_ASSERT_TRUE_MESSAGE(maxAbs > 0.5f && maxAbs <= 3.0f, "small gaze drift");
  TEST_ASSERT_TRUE_MESSAGE(maxStep < 1.0f, "less than a pixel per frame: no jitter");
}

void test_talking_mouth_follows_level_without_flicker() {
  FaceAnimator a;
  a.reset(0, 1);
  ViewModel vm = vmFor(Expression::Speaking);
  vm.state = RobotState::Speaking;
  for (uint32_t t = 0; t < 400; t += 33) a.update(vm, t);  // settle into the expression
  uint32_t lastChange = 0;
  float last = -1;
  uint32_t minHold = 0xFFFFFFFF;
  float maxOpen = 0;
  for (uint32_t t = 400; t < 6000; t += 10) {
    vm.audioLevel = (uint8_t)((t / 50) % 2 ? 95 : 5);  // a harsh, fast-switching level
    FacePose p = a.update(vm, t);
    TEST_ASSERT_EQUAL((int)MouthKind::Talk, (int)p.mouth);
    if (p.mouthOpen != last) {
      if (last >= 0 && t - lastChange < minHold) minHold = t - lastChange;
      lastChange = t;
      last = p.mouthOpen;
    }
    if (p.mouthOpen > maxOpen) maxOpen = p.mouthOpen;
  }
  TEST_ASSERT_TRUE_MESSAGE(minHold >= 70, "each mouth shape is held >= 70 ms");
  TEST_ASSERT_TRUE(maxOpen > 0.3f);
  vm.audioLevel = 0;
  vm.expression = Expression::Idle;
  for (uint32_t t = 6000; t < 7000; t += 33) a.update(vm, t);
  TEST_ASSERT_EQUAL((int)MouthKind::Grin, (int)a.update(vm, 7000).mouth);  // BMO's resting open mouth
}

void test_expression_change_happens_behind_a_blink() {
  FaceAnimator a;
  a.reset(0, 9);
  ViewModel vm = vmFor(Expression::Idle);
  for (uint32_t t = 0; t < 1000; t += 33) a.update(vm, t);
  uint32_t before = a.blinkCount();
  vm.expression = Expression::Happy;
  FacePose first = a.update(vm, 1000);
  TEST_ASSERT_EQUAL_UINT32(before + 1, a.blinkCount());
  TEST_ASSERT_EQUAL((int)MouthKind::Grin, (int)first.mouth);  // old face until the eyes close
  FacePose p;
  for (uint32_t t = 1033; t < 1400; t += 33) p = a.update(vm, t);
  TEST_ASSERT_EQUAL((int)MouthKind::BigGrin, (int)p.mouth);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, p.eye[0].open);  // and open again
}

void test_excited_bounces_softly_and_sleep_dims_the_screen() {
  FaceAnimator a;
  a.reset(0, 2);
  ViewModel vm = vmFor(Expression::Excited);
  float maxBob = 0;
  for (uint32_t t = 0; t < 3000; t += 33) {
    FacePose p = a.update(vm, t);
    if (fabsf(p.bob) > maxBob) maxBob = fabsf(p.bob);
  }
  TEST_ASSERT_TRUE(maxBob > 0.5f && maxBob <= 1.0f);
  vm.expression = Expression::Sleeping;
  FacePose p;
  for (uint32_t t = 3000; t < 3500; t += 33) p = a.update(vm, t);
  TEST_ASSERT_TRUE(p.darkScreen);
  TEST_ASSERT_TRUE(p.marks & kMarkZzz);
}

void test_talking_keeps_the_reply_mood() {
  FaceAnimator a;
  a.reset(0, 4);
  ViewModel vm = vmFor(Expression::Speaking);
  vm.state = RobotState::Speaking;
  vm.mood = Expression::Laughing;
  vm.audioLevel = 70;
  FacePose p;
  for (uint32_t t = 0; t < 800; t += 33) p = a.update(vm, t);
  TEST_ASSERT_EQUAL((int)EyeKind::Squeeze, (int)p.eye[0].kind);  // laughing eyes...
  TEST_ASSERT_EQUAL((int)MouthKind::Talk, (int)p.mouth);         // ...and a talking mouth
  TEST_ASSERT_TRUE(p.marks & kMarkJoyTears);
  vm.mood = Expression::Speaking;  // not a mood: plain talking face
  for (uint32_t t = 800; t < 1200; t += 33) p = a.update(vm, t);
  TEST_ASSERT_EQUAL((int)EyeKind::Dot, (int)p.eye[0].kind);
}

void test_scared_trembles_and_laughing_bobs_within_a_pixel_or_two() {
  FaceAnimator a;
  a.reset(0, 5);
  ViewModel vm = vmFor(Expression::Scared);
  float maxShake = 0, maxBob = 0;
  bool left = false, right = false;
  for (uint32_t t = 0; t < 2000; t += 33) {
    FacePose p = a.update(vm, t);
    if (fabsf(p.shake) > maxShake) maxShake = fabsf(p.shake);
    left |= p.shake < 0;
    right |= p.shake > 0;
  }
  TEST_ASSERT_TRUE(left && right && maxShake <= 1.0f);
  vm.expression = Expression::Laughing;
  for (uint32_t t = 2000; t < 4000; t += 33) {
    FacePose p = a.update(vm, t);
    if (fabsf(p.bob) > maxBob) maxBob = fabsf(p.bob);
  }
  TEST_ASSERT_TRUE(maxBob > 0.5f && maxBob <= 1.5f);
}

// ---- the painter (face_canvas): the same drawing for the SH1106 and a colour display
struct ToneGrid : Surface {
  static const int W = 128 * 3, H = 64 * 3;
  uint8_t t[W * H];
  int w, h, outside = 0;
  ToneGrid(int ww, int hh) : w(ww), h(hh) { memset(t, (int)Tone::Screen, sizeof(t)); }
  int width() const override { return w; }
  int height() const override { return h; }
  void plot(int x, int y, Tone tone) override {
    if (x < 0 || y < 0 || x >= w || y >= h) { outside++; return; }
    t[y * w + x] = (uint8_t)tone;
  }
  Tone at(int x, int y) const { return (Tone)t[y * w + x]; }
  int count(Tone tone) const {
    int n = 0;
    for (int i = 0; i < w * h; i++) n += t[i] == (uint8_t)tone;
    return n;
  }
};

void test_bmo_idle_face_has_dot_eyes_and_an_open_mouth_with_teeth_and_tongue() {
  ToneGrid g(128, 64);
  FacePainter(g).draw(designFor(Expression::Idle));
  FaceLayout l;
  TEST_ASSERT_EQUAL((int)Tone::Ink, (int)g.at((int)l.eyeX[0], (int)l.eyeY));  // eye centres are ink
  TEST_ASSERT_EQUAL((int)Tone::Ink, (int)g.at((int)l.eyeX[1], (int)l.eyeY));
  TEST_ASSERT_EQUAL((int)Tone::Screen, (int)g.at(64, 25));  // between the eyes: screen
  TEST_ASSERT_TRUE(g.count(Tone::Lip) > 20);
  TEST_ASSERT_TRUE(g.count(Tone::Mouth) > 30);
  TEST_ASSERT_TRUE(g.count(Tone::Teeth) > 15);
  TEST_ASSERT_TRUE(g.count(Tone::Tongue) > 10);
  // teeth sit above the tongue, both inside the mouth
  int teethY = -1, tongueY = -1;
  for (int y = 0; y < 64; y++) {
    if (teethY < 0 && g.at(64, y) == Tone::Teeth) teethY = y;
    if (tongueY < 0 && g.at(64, y) == Tone::Tongue) tongueY = y;
  }
  TEST_ASSERT_TRUE(teethY > 30 && tongueY > teethY);
  // symmetric around the mouth's centre line
  for (int y = 30; y < 60; y++)
    for (int x = 40; x < 64; x++) TEST_ASSERT_EQUAL((int)g.at(x, y), (int)g.at(127 - x, y));
}

void test_every_expression_paints_inside_the_screen_at_any_scale() {
  for (int e = 0; e < (int)Expression::kCount; e++) {
    FacePose p = designFor((Expression)e);
    p.phase = 1234;
    p.level = 80;
    p.mouthOpen = 1;
    p.shake = 1;
    p.bob = -1;
    p.marks |= kMarkMicOff | kMarkWifiOff;
    ToneGrid one(128, 64), three(ToneGrid::W, ToneGrid::H);
    FacePainter(one).draw(p);
    FacePainter(three, 3).draw(p);
    TEST_ASSERT_EQUAL_MESSAGE(0, one.outside, toString((Expression)e));
    TEST_ASSERT_EQUAL_MESSAGE(0, three.outside, toString((Expression)e));
    // the 3x drawing is the same face, not a different one: ink covers ~9x the area
    int a = one.count(Tone::Ink), b = three.count(Tone::Ink);
    if (a > 30) TEST_ASSERT_TRUE_MESSAGE(b > 6 * a && b < 12 * a, toString((Expression)e));
  }
}

void test_tones_map_to_bmo_colours_and_to_sh1106_pixels() {
  Rgb screen = bmoColor(Tone::Screen), ink = bmoColor(Tone::Ink), mouth = bmoColor(Tone::Mouth);
  TEST_ASSERT_TRUE(screen.g > screen.r && screen.g > screen.b && screen.g > 200);  // BMO's mint green
  TEST_ASSERT_TRUE(ink.r < 40 && ink.g < 40 && ink.b < 40);
  TEST_ASSERT_TRUE(mouth.g > mouth.r && mouth.g > mouth.b);  // green inside the mouth
  Rgb teeth = bmoColor(Tone::Teeth);
  TEST_ASSERT_TRUE(teeth.r > 240 && teeth.g > 240 && teeth.b > 240);
  TEST_ASSERT_EQUAL_HEX16(0xF800, toRgb565(Rgb{255, 0, 0}));
  // SH1106: screen and teeth unlit, features lit, soft parts half-tone
  TEST_ASSERT_EQUAL(0, monoValue(Tone::Screen, 3, 4));
  TEST_ASSERT_EQUAL(0, monoValue(Tone::Teeth, 3, 4));
  TEST_ASSERT_EQUAL(1, monoValue(Tone::Ink, 3, 4));
  TEST_ASSERT_EQUAL(1, monoValue(Tone::Lip, 3, 4));
  TEST_ASSERT_TRUE(monoValue(Tone::Tongue, 0, 0) != monoValue(Tone::Tongue, 1, 0));  // dithered
  // sleeping dims the colour screen
  uint8_t px[3];
  RgbSurface s(px, 1, 1);
  s.plot(0, 0, Tone::Screen);
  uint8_t bright = px[1];
  s.setDim(true);
  s.plot(0, 0, Tone::Screen);
  TEST_ASSERT_TRUE(px[1] < bright / 2);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_every_expression_has_its_own_design);
  RUN_TEST(test_expression_names_round_trip);
  RUN_TEST(test_blinks_are_soft_and_periodic);
  RUN_TEST(test_animation_is_deterministic);
  RUN_TEST(test_eyes_drift_slightly_and_never_jump);
  RUN_TEST(test_talking_mouth_follows_level_without_flicker);
  RUN_TEST(test_expression_change_happens_behind_a_blink);
  RUN_TEST(test_excited_bounces_softly_and_sleep_dims_the_screen);
  RUN_TEST(test_talking_keeps_the_reply_mood);
  RUN_TEST(test_scared_trembles_and_laughing_bobs_within_a_pixel_or_two);
  RUN_TEST(test_bmo_idle_face_has_dot_eyes_and_an_open_mouth_with_teeth_and_tongue);
  RUN_TEST(test_every_expression_paints_inside_the_screen_at_any_scale);
  RUN_TEST(test_tones_map_to_bmo_colours_and_to_sh1106_pixels);
  return UNITY_END();
}
