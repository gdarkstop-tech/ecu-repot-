#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "offline_content.h"
#include "offline_tutor.h"
#include "utf8.h"

using namespace robot;

struct FakePlatform : IRobotPlatform {
  void writeLine(const char*) override {}
  void getInfo(DeviceInfo&) override {}
  bool runCommand(const DebugCommand&, char*, size_t) override { return false; }
};

void setUp() {}
void tearDown() {}

static void boot(Robot& r) {
  r.begin(0);
  for (uint32_t t = 0; t < 2500; t += 50) r.tick(t);
}

void test_alphabet_en_starts_at_A() {
  FakePlatform p;
  Robot r(p);
  boot(r);
  OfflineTutor ot;
  ot.start(r, 3000);
  TEST_ASSERT_EQUAL((int)RobotState::Learning, (int)r.view(3000).state);
  TEST_ASSERT_EQUAL_STRING("A", r.view(3000).contentValue);
  TEST_ASSERT_EQUAL_STRING("A is for Apple", r.view(3000).caption);
  ot.next(r, 3100);
  TEST_ASSERT_EQUAL_STRING("B", r.view(3100).contentValue);
}

void test_activity_cycle_and_arabic_is_rtl_valid_utf8() {
  FakePlatform p;
  Robot r(p);
  boot(r);
  OfflineTutor ot;
  ot.start(r, 3000);
  ot.nextActivity(r, 3100);
  TEST_ASSERT_EQUAL((int)OfflineTutor::Activity::AlphabetAr, (int)ot.activity());
  TEST_ASSERT_TRUE(r.view(3100).captionRtl);
  for (int i = 0; i < offline::kArLetters; i++) {
    TEST_ASSERT_TRUE(utf8IsValid(offline::kArAlphabet[i].letter));
    TEST_ASSERT_TRUE(utf8IsValid(offline::kArAlphabet[i].caption));
    TEST_ASSERT_TRUE(strlen(offline::kArAlphabet[i].caption) < kMaxTextBytes);
  }
}

void test_counting_wraps_after_100() {
  FakePlatform p;
  Robot r(p);
  boot(r);
  OfflineTutor ot;
  ot.start(r, 3000);
  ot.nextActivity(r, 3000);
  ot.nextActivity(r, 3000);
  TEST_ASSERT_EQUAL_STRING("1", r.view(3000).contentValue);
  TEST_ASSERT_EQUAL_STRING("one", r.view(3000).caption);
  for (int i = 0; i < 99; i++) ot.next(r, 3000);
  TEST_ASSERT_EQUAL_STRING("100", r.view(3000).contentValue);
  TEST_ASSERT_EQUAL_STRING("one hundred", r.view(3000).caption);
  ot.next(r, 3000);
  TEST_ASSERT_EQUAL_STRING("1", r.view(3000).contentValue);
  TEST_ASSERT_EQUAL_STRING("seventy-three", offline::kNumberWords[73]);
}

void test_math_answers_are_correct_and_age_appropriate() {
  for (int age = 4; age <= 10; age++) {
    FakePlatform p;
    Robot r(p);
    boot(r);
    OfflineTutor ot;
    ot.setAge((uint8_t)age);
    ot.seed(1234 + age);
    ot.start(r, 3000);
    for (int i = 0; i < 3; i++) ot.nextActivity(r, 3000);
    TEST_ASSERT_EQUAL((int)OfflineTutor::Activity::Math, (int)ot.activity());
    for (int k = 0; k < 200; k++) {
      int a = ot.a(), b = ot.b();
      char op = ot.op();
      int expect = op == '+' ? a + b : (op == '-' ? a - b : a * b);
      TEST_ASSERT_EQUAL(expect, ot.answer());
      TEST_ASSERT_TRUE(ot.answer() >= 0);  // never negative for children
      if (age <= 5) {
        TEST_ASSERT_EQUAL('+', op);
        TEST_ASSERT_TRUE(a <= 5 && b <= 5);
      }
      ot.next(r, 3000);  // reveal
      char expectStr[32];
      snprintf(expectStr, sizeof(expectStr), "%d %c %d = %d", a, op, b, expect);
      TEST_ASSERT_EQUAL_STRING(expectStr, r.view(3000).contentValue);
      ot.next(r, 3000);  // new problem
    }
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_alphabet_en_starts_at_A);
  RUN_TEST(test_activity_cycle_and_arabic_is_rtl_valid_utf8);
  RUN_TEST(test_counting_wraps_after_100);
  RUN_TEST(test_math_answers_are_correct_and_age_appropriate);
  return UNITY_END();
}
