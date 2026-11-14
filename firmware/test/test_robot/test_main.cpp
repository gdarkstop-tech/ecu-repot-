#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "robot.h"

using namespace robot;

struct FakePlatform : IRobotPlatform {
  std::vector<std::string> lines;
  std::vector<std::string> hwCommands;
  void writeLine(const char* l) override { lines.push_back(l); }
  void getInfo(DeviceInfo& i) override {
    i.fw = "test";
    i.board = "native";
    i.oledOk = true;
    i.oledAddr = 0x3C;
  }
  bool runCommand(const DebugCommand& c, char* err, size_t n) override {
    hwCommands.push_back(toString(c.type));
    if (c.type == CmdType::Reboot) {
      snprintf(err, n, "not_in_test");
      return false;
    }
    return true;
  }
  bool saw(const char* needle) const {
    for (auto& l : lines)
      if (l.find(needle) != std::string::npos) return true;
    return false;
  }
  void clear() { lines.clear(); }
};

static StateConfig cfg() {
  StateConfig c;
  c.startupMs = 1000;
  return c;
}

static void send(Robot& r, const char* s, uint32_t now) { r.onDebugLine(s, strlen(s), now); }

void setUp() {}
void tearDown() {}

void test_hello_on_begin_and_muted_after_startup() {
  FakePlatform p;
  Robot r(p, cfg());
  r.begin(0);
  TEST_ASSERT_TRUE(p.saw("\"evt\":\"hello\""));
  r.tick(1000);
  TEST_ASSERT_EQUAL((int)RobotState::Muted, (int)r.view(1000).state);
  TEST_ASSERT_EQUAL((int)Expression::Muted, (int)r.view(1000).expression);
  TEST_ASSERT_TRUE(p.saw("\"to\":\"MUTED\""));
}

void test_bad_line_gets_nack_with_id() {
  FakePlatform p;
  Robot r(p, cfg());
  r.begin(0);
  p.clear();
  send(r, "{\"id\":9,\"cmd\":\"nope\"}", 10);
  TEST_ASSERT_TRUE(p.saw("\"id\":9"));
  TEST_ASSERT_TRUE(p.saw("\"ok\":false"));
  TEST_ASSERT_TRUE(p.saw("unknown_cmd"));
}

void test_face_override_expires() {
  FakePlatform p;
  Robot r(p, cfg());
  r.begin(0);
  r.tick(1000);
  send(r, "{\"cmd\":\"face\",\"expr\":\"happy\",\"ms\":500}", 1100);
  TEST_ASSERT_EQUAL((int)Expression::Happy, (int)r.view(1200).expression);
  r.tick(1600);
  TEST_ASSERT_EQUAL((int)Expression::Muted, (int)r.view(1600).expression);
}

void test_listen_rejected_when_muted_then_allowed() {
  FakePlatform p;
  Robot r(p, cfg());
  r.begin(0);
  r.tick(1000);
  p.clear();
  send(r, "{\"id\":1,\"cmd\":\"state\",\"state\":\"LISTENING\"}", 1100);
  TEST_ASSERT_TRUE(p.saw("mic_muted"));
  send(r, "{\"id\":2,\"cmd\":\"mute\",\"on\":false}", 1200);
  send(r, "{\"id\":3,\"cmd\":\"state\",\"state\":\"LISTENING\"}", 1300);
  TEST_ASSERT_EQUAL((int)RobotState::Listening, (int)r.view(1300).state);
}

void test_show_content_enters_learning_and_times_out() {
  FakePlatform p;
  Robot r(p, cfg());
  r.begin(0);
  r.tick(1000);
  send(r, "{\"cmd\":\"show\",\"kind\":\"number\",\"value\":\"7\",\"ms\":2000}", 1100);
  const ViewModel& v = r.view(1200);
  TEST_ASSERT_EQUAL((int)RobotState::Learning, (int)v.state);
  TEST_ASSERT_EQUAL((int)ContentKind::Number, (int)v.contentKind);
  TEST_ASSERT_EQUAL_STRING("7", v.contentValue);
  r.tick(3100);
  TEST_ASSERT_EQUAL((int)RobotState::Muted, (int)r.view(3100).state);
  TEST_ASSERT_EQUAL((int)ContentKind::None, (int)r.view(3100).contentKind);
}

void test_say_sets_marquee_then_clears_after_speaking() {
  FakePlatform p;
  Robot r(p, cfg());
  r.begin(0);
  r.tick(1000);
  send(r, "{\"cmd\":\"say\",\"text\":\"Seven plus five is twelve\",\"ms\":1000}", 1100);
  TEST_ASSERT_EQUAL((int)RobotState::Speaking, (int)r.view(1100).state);
  TEST_ASSERT_EQUAL_STRING("Seven plus five is twelve", r.view(1100).marquee);
  r.tick(2100);
  TEST_ASSERT_EQUAL((int)RobotState::Muted, (int)r.view(2100).state);
  TEST_ASSERT_EQUAL_STRING("", r.view(2100).marquee);
}

void test_button_toggles_mute() {
  FakePlatform p;
  Robot r(p, cfg());
  r.begin(0);
  r.tick(1000);
  r.onMuteButton(1100);
  TEST_ASSERT_EQUAL((int)RobotState::Idle, (int)r.view(1100).state);
  TEST_ASSERT_TRUE(p.saw("\"evt\":\"mute\",\"on\":false"));
  r.onMuteButton(1200);
  TEST_ASSERT_EQUAL((int)RobotState::Muted, (int)r.view(1200).state);
}

void test_hardware_commands_delegated_and_errors_reported() {
  FakePlatform p;
  Robot r(p, cfg());
  r.begin(0);
  r.tick(1000);
  send(r, "{\"id\":4,\"cmd\":\"mic_test\",\"ms\":1000}", 1100);
  send(r, "{\"id\":5,\"cmd\":\"reboot\"}", 1100);
  TEST_ASSERT_EQUAL(2, (int)p.hwCommands.size());
  TEST_ASSERT_TRUE(p.saw("not_in_test"));
}

void test_status_never_contains_token() {
  FakePlatform p;
  Robot r(p, cfg());
  r.begin(0);
  send(r, "{\"cmd\":\"status\"}", 10);
  TEST_ASSERT_TRUE(p.saw("\"token_set\":false"));
  TEST_ASSERT_FALSE(p.saw("device_token"));
}

void test_selftest_runs_all_steps_and_finishes() {
  FakePlatform p;
  Robot r(p, cfg());
  r.begin(0);
  r.tick(1000);
  send(r, "{\"cmd\":\"selftest\"}", 1100);
  uint32_t t = 1100;
  for (int i = 0; i < 2000 && !p.saw("\"done\":true"); i++) {
    t += 50;
    r.tick(t);
    r.view(t);
  }
  TEST_ASSERT_TRUE(p.saw("\"name\":\"happy\""));
  TEST_ASSERT_TRUE(p.saw("\"name\":\"math\""));
  TEST_ASSERT_TRUE(p.saw("\"done\":true"));
  TEST_ASSERT_EQUAL((int)RobotState::Muted, (int)r.view(t).state);
}

void test_read_command_shows_text_and_clears_after_speaking() {
  FakePlatform p;
  Robot r(p, cfg());
  r.begin(0);
  r.tick(1000);
  send(r, "{\"cmd\":\"read\",\"text\":\"Hello there, this is a test answer.\",\"ms\":3000}", 1100);
  TEST_ASSERT_EQUAL((int)RobotState::Speaking, (int)r.view(1200).state);
  TEST_ASSERT_EQUAL_STRING("Hello there, this is a test answer.", r.view(1200).readingText);
  TEST_ASSERT_EQUAL_UINT32(3000, r.view(1200).readingDurationMs);
  r.tick(4700);  // 3000 + 500 ms deadline
  TEST_ASSERT_EQUAL((int)RobotState::Muted, (int)r.view(4700).state);
  TEST_ASSERT_EQUAL_STRING("", r.view(4700).readingText);
}

void test_overflow_reports_error() {
  FakePlatform p;
  Robot r(p, cfg());
  r.begin(0);
  p.clear();
  r.onDebugOverflow(5);
  TEST_ASSERT_TRUE(p.saw("line_too_long"));
}

void test_sleeping_face_after_inactivity_and_wake_on_command() {
  FakePlatform p;
  StateConfig c = cfg();
  c.sleepAfterMs = 10000;
  Robot r(p, c);
  r.begin(0);
  r.tick(1000);
  TEST_ASSERT_EQUAL((int)Expression::Sleeping, (int)r.view(10001).expression);
  send(r, "{\"cmd\":\"ping\"}", 10002);  // heartbeats must not wake it
  TEST_ASSERT_EQUAL((int)Expression::Sleeping, (int)r.view(10003).expression);
  send(r, "{\"cmd\":\"face\",\"expr\":\"blink\"}", 10004);
  TEST_ASSERT_EQUAL((int)Expression::Muted, (int)r.view(10005).expression);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_hello_on_begin_and_muted_after_startup);
  RUN_TEST(test_bad_line_gets_nack_with_id);
  RUN_TEST(test_face_override_expires);
  RUN_TEST(test_listen_rejected_when_muted_then_allowed);
  RUN_TEST(test_show_content_enters_learning_and_times_out);
  RUN_TEST(test_say_sets_marquee_then_clears_after_speaking);
  RUN_TEST(test_button_toggles_mute);
  RUN_TEST(test_hardware_commands_delegated_and_errors_reported);
  RUN_TEST(test_status_never_contains_token);
  RUN_TEST(test_selftest_runs_all_steps_and_finishes);
  RUN_TEST(test_read_command_shows_text_and_clears_after_speaking);
  RUN_TEST(test_overflow_reports_error);
  RUN_TEST(test_sleeping_face_after_inactivity_and_wake_on_command);
  return UNITY_END();
}
