// The BMO-inspired face model: every expression is a distinct design, and the animation is
// soft and stable (periodic blinks, no jitter, a talking mouth that does not flicker).
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

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
                            Expression::Offline,  Expression::Error};
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
}

void test_expression_names_round_trip() {
  const char* names[] = {"excited", "curious", "confused", "worried", "playful", "proud", "offline", "sleepy"};
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
  return UNITY_END();
}
