#include <unity.h>

#include "state_machine.h"

using namespace robot;

static StateConfig cfg() {
  StateConfig c;
  c.startupMs = 1000;
  c.processingTimeoutMs = 5000;
  c.speakingTimeoutMs = 9000;
  c.errorRecoverMs = 3000;
  c.sleepAfterMs = 60000;
  return c;
}

void setUp() {}
void tearDown() {}

void test_startup_is_muted_and_ends_in_muted() {
  StateMachine sm(cfg());
  sm.begin(0);
  TEST_ASSERT_EQUAL((int)RobotState::Startup, (int)sm.state());
  TEST_ASSERT_TRUE(sm.muted());
  sm.tick(999);
  TEST_ASSERT_EQUAL((int)RobotState::Startup, (int)sm.state());
  sm.tick(1000);
  TEST_ASSERT_EQUAL((int)RobotState::Muted, (int)sm.state());
  TEST_ASSERT_EQUAL((int)TransitionReason::StartupDone, (int)sm.lastReason());
}

void test_requests_rejected_during_startup_except_error() {
  StateMachine sm(cfg());
  sm.begin(0);
  TEST_ASSERT_EQUAL((int)TransitionResult::RejectedStartup, (int)sm.request(RobotState::Speaking, 10));
  sm.raiseError("boom", true, 20);
  TEST_ASSERT_EQUAL((int)RobotState::Error, (int)sm.state());
  TEST_ASSERT_EQUAL_STRING("boom", sm.errorCode());
}

void test_cannot_request_startup_or_muted() {
  StateMachine sm(cfg());
  sm.begin(0);
  sm.tick(1000);
  TEST_ASSERT_EQUAL((int)TransitionResult::RejectedInvalid, (int)sm.request(RobotState::Startup, 1100));
  TEST_ASSERT_EQUAL((int)TransitionResult::RejectedInvalid, (int)sm.request(RobotState::Muted, 1100));
}

void test_listening_rejected_while_muted() {
  StateMachine sm(cfg());
  sm.begin(0);
  sm.tick(1000);
  TEST_ASSERT_EQUAL((int)TransitionResult::RejectedMuted, (int)sm.request(RobotState::Listening, 1100));
  sm.setMuted(false, 1200);
  TEST_ASSERT_EQUAL((int)RobotState::Idle, (int)sm.state());
  TEST_ASSERT_EQUAL((int)TransitionResult::Ok, (int)sm.request(RobotState::Listening, 1300));
}

void test_idle_request_resolves_to_rest_state() {
  StateMachine sm(cfg());
  sm.begin(0);
  sm.tick(1000);
  sm.request(RobotState::Speaking, 1100);
  sm.request(RobotState::Idle, 1200);
  TEST_ASSERT_EQUAL((int)RobotState::Muted, (int)sm.state());  // still muted
}

void test_mute_while_listening_stops_capture() {
  StateMachine sm(cfg());
  sm.begin(0);
  sm.tick(1000);
  sm.setMuted(false, 1000);
  sm.request(RobotState::Listening, 1100);
  sm.setMuted(true, 1200);
  TEST_ASSERT_EQUAL((int)RobotState::Muted, (int)sm.state());
}

void test_mute_while_speaking_keeps_speaking_then_rests_muted() {
  StateMachine sm(cfg());
  sm.begin(0);
  sm.tick(1000);
  sm.setMuted(false, 1000);
  sm.request(RobotState::Speaking, 1100, 2000);
  sm.setMuted(true, 1200);
  TEST_ASSERT_EQUAL((int)RobotState::Speaking, (int)sm.state());
  sm.tick(3100);
  TEST_ASSERT_EQUAL((int)RobotState::Muted, (int)sm.state());
  TEST_ASSERT_EQUAL((int)TransitionReason::Timeout, (int)sm.lastReason());
}

void test_processing_times_out_to_rest() {
  StateMachine sm(cfg());
  sm.begin(0);
  sm.tick(1000);
  sm.setMuted(false, 1000);
  sm.request(RobotState::Processing, 2000);
  sm.tick(6999);
  TEST_ASSERT_EQUAL((int)RobotState::Processing, (int)sm.state());
  sm.tick(7000);
  TEST_ASSERT_EQUAL((int)RobotState::Idle, (int)sm.state());
}

void test_extend_deadline_keeps_state_alive() {
  StateMachine sm(cfg());
  sm.begin(0);
  sm.tick(1000);
  sm.request(RobotState::Processing, 2000);  // deadline 7000
  sm.extendDeadline(6000, 5000);             // -> 11000
  sm.tick(10999);
  TEST_ASSERT_EQUAL((int)RobotState::Processing, (int)sm.state());
  sm.tick(11000);
  TEST_ASSERT_EQUAL((int)RobotState::Muted, (int)sm.state());
}

void test_recoverable_error_clears_itself() {
  StateMachine sm(cfg());
  sm.begin(0);
  sm.tick(1000);
  sm.raiseError("gateway_timeout", true, 2000);
  sm.tick(4999);
  TEST_ASSERT_EQUAL((int)RobotState::Error, (int)sm.state());
  sm.tick(5000);
  TEST_ASSERT_EQUAL((int)RobotState::Muted, (int)sm.state());
  TEST_ASSERT_EQUAL_STRING("", sm.errorCode());
}

void test_fatal_error_is_sticky() {
  StateMachine sm(cfg());
  sm.begin(0);
  sm.tick(1000);
  sm.raiseError("auth_rejected", false, 2000);
  sm.tick(2000000);
  TEST_ASSERT_EQUAL((int)RobotState::Error, (int)sm.state());
}

void test_millis_wraparound_deadline() {
  StateMachine sm(cfg());
  uint32_t t0 = 0xFFFFF000u;
  sm.begin(t0);
  sm.tick(t0 + 1000);  // wraps past zero
  TEST_ASSERT_EQUAL((int)RobotState::Muted, (int)sm.state());
}

void test_sleep_after_inactivity_only_in_rest() {
  StateMachine sm(cfg());
  sm.begin(0);
  sm.tick(1000);
  TEST_ASSERT_FALSE(sm.sleeping(59999));
  TEST_ASSERT_TRUE(sm.sleeping(60000));
  sm.noteActivity(60000);
  TEST_ASSERT_FALSE(sm.sleeping(60001));
  sm.request(RobotState::Learning, 61000, UINT32_MAX);
  TEST_ASSERT_FALSE(sm.sleeping(500000));
}

static int g_calls = 0;
static RobotState g_lastTo;
static void listener(RobotState, RobotState to, TransitionReason, void*) {
  g_calls++;
  g_lastTo = to;
}

void test_listener_called_on_each_transition() {
  StateMachine sm(cfg());
  g_calls = 0;
  sm.setListener(listener, nullptr);
  sm.begin(0);
  sm.tick(1000);
  sm.request(RobotState::Speaking, 1100);
  TEST_ASSERT_EQUAL(2, g_calls);
  TEST_ASSERT_EQUAL((int)RobotState::Speaking, (int)g_lastTo);
  sm.request(RobotState::Speaking, 1200);  // unchanged: no callback
  TEST_ASSERT_EQUAL(2, g_calls);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_startup_is_muted_and_ends_in_muted);
  RUN_TEST(test_requests_rejected_during_startup_except_error);
  RUN_TEST(test_cannot_request_startup_or_muted);
  RUN_TEST(test_listening_rejected_while_muted);
  RUN_TEST(test_idle_request_resolves_to_rest_state);
  RUN_TEST(test_mute_while_listening_stops_capture);
  RUN_TEST(test_mute_while_speaking_keeps_speaking_then_rests_muted);
  RUN_TEST(test_processing_times_out_to_rest);
  RUN_TEST(test_extend_deadline_keeps_state_alive);
  RUN_TEST(test_recoverable_error_clears_itself);
  RUN_TEST(test_fatal_error_is_sticky);
  RUN_TEST(test_millis_wraparound_deadline);
  RUN_TEST(test_sleep_after_inactivity_only_in_rest);
  RUN_TEST(test_listener_called_on_each_transition);
  return UNITY_END();
}
