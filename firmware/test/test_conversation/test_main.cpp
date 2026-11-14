#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "conversation.h"
#include "utf8.h"

using namespace robot;

struct FakePlatform : IRobotPlatform {
  std::vector<std::string> lines;
  void writeLine(const char* l) override { lines.push_back(l); }
  void getInfo(DeviceInfo& i) override { i.fw = "t"; }
  bool runCommand(const DebugCommand&, char*, size_t) override { return false; }
  bool saw(const char* s) const {
    for (auto& l : lines)
      if (l.find(s) != std::string::npos) return true;
    return false;
  }
};

struct FakeGateway : IGateway {
  bool isReady = true;
  int submitted = 0;
  size_t lastSamples = 0;
  GatewayPoll next = GatewayPoll::Busy;
  GatewayReply reply;
  const char* err = "";
  bool recoverable = true;
  int cancels = 0;
  bool ready() const override { return isReady; }
  bool startUtterance(const int16_t*, size_t n, uint32_t) override { submitted++; lastSamples = n; return true; }
  bool startText(const char*, const char*) override { submitted++; return true; }
  GatewayPoll poll(GatewayReply& r, char* e, size_t n, bool& rec) override {
    if (next == GatewayPoll::Reply) r = reply;
    if (next == GatewayPoll::Failed) { snprintf(e, n, "%s", err); rec = recoverable; }
    GatewayPoll p = next;
    if (p == GatewayPoll::Reply || p == GatewayPoll::Failed) next = GatewayPoll::Idle;
    return p;
  }
  void cancel() override { cancels++; }
};

struct FakeAudio : IAudioOutput {
  bool playing = false;
  int stops = 0;
  bool active() const override { return playing; }
  uint8_t level() const override { return playing ? 60 : 0; }
  void stop() override { playing = false; stops++; }
};

static int16_t store[16000 * 12];
static const size_t N = 320;

struct Rig {
  FakePlatform p;
  StateConfig sc;
  Robot robot{p, sc};
  Vad vad;
  UtteranceBuffer buf;
  FakeGateway gw;
  FakeAudio audio;
  Conversation conv{robot, vad, buf, gw, audio};
  uint32_t t = 0;
  int frame = 0;
  Rig() {
    buf.attach(store, sizeof(store) / sizeof(store[0]));
    robot.begin(0);
    for (; t < 2500; t += 20) robot.tick(t);
  }
  void step(bool loud) {
    int16_t f[N];
    for (size_t i = 0; i < N; i++) {
      double s = loud ? 4000 * sin(2 * M_PI * 220 * ((frame * N + i) / 16000.0)) : 0;
      f[i] = (int16_t)(s + ((int)((i * 7919 + frame * 104729) % 61) - 30));
    }
    frame++;
    t += 20;
    conv.onMicFrame(f, N, t);
    robot.tick(t);
    conv.tick(t);
  }
  void run(bool loud, int frames) {
    for (int i = 0; i < frames; i++) step(loud);
  }
  RobotState state() { return robot.view(t).state; }
};

void setUp() {}
void tearDown() {}

void test_muted_robot_never_listens_or_submits() {
  Rig r;
  r.run(false, 50);
  r.run(true, 60);
  r.run(false, 60);
  TEST_ASSERT_EQUAL(0, r.gw.submitted);
  TEST_ASSERT_EQUAL((int)RobotState::Muted, (int)r.state());
}

void test_full_turn_listen_process_speak_and_rearm() {
  Rig r;
  r.robot.setMuted(false, r.t);
  r.run(false, 50);
  r.run(true, 10);
  TEST_ASSERT_EQUAL((int)RobotState::Listening, (int)r.state());
  r.run(true, 40);
  r.run(false, 45);  // end-of-speech hangover
  TEST_ASSERT_EQUAL(1, r.gw.submitted);
  TEST_ASSERT_TRUE(r.gw.lastSamples > 16000);  // ~1 s + pre-roll
  TEST_ASSERT_EQUAL((int)RobotState::Processing, (int)r.state());

  // Gateway replies; audio starts streaming.
  r.gw.reply.hasAudio = true;
  snprintf(r.gw.reply.oledText, sizeof(r.gw.reply.oledText), "7 + 5 = 12");
  r.gw.next = GatewayPoll::Reply;
  r.audio.playing = true;
  r.run(false, 1);
  TEST_ASSERT_EQUAL((int)RobotState::Speaking, (int)r.state());
  TEST_ASSERT_EQUAL_STRING("7 + 5 = 12", r.robot.view(r.t).marquee);

  // While speaking, even loud sound (our own speaker) must not start a new capture.
  r.run(true, 50);
  TEST_ASSERT_EQUAL(1, r.gw.submitted);
  TEST_ASSERT_EQUAL((int)RobotState::Speaking, (int)r.state());

  // Playback ends -> echo tail -> follow-up listening.
  r.audio.playing = false;
  r.run(false, 5);
  TEST_ASSERT_EQUAL((int)Conversation::Phase::Tail, (int)r.conv.phase());
  r.run(false, 25);
  TEST_ASSERT_EQUAL((int)Conversation::Phase::Waiting, (int)r.conv.phase());
  TEST_ASSERT_EQUAL((int)RobotState::Listening, (int)r.state());
  // Follow-up window closes back to IDLE.
  r.run(false, 450);
  TEST_ASSERT_EQUAL((int)RobotState::Idle, (int)r.state());
}

void test_sound_during_echo_tail_is_ignored() {
  Rig r;
  r.robot.setMuted(false, r.t);
  TEST_ASSERT_TRUE(r.conv.submitText("hello", "en", r.t));
  r.gw.reply.hasAudio = true;
  r.gw.next = GatewayPoll::Reply;
  r.audio.playing = true;
  r.run(false, 2);
  r.audio.playing = false;
  r.run(true, 15);  // speaker reverb right after playback, inside the 400 ms tail
  TEST_ASSERT_EQUAL(1, r.gw.submitted);
  TEST_ASSERT_FALSE(r.vad.inSpeech());
}

void test_gateway_failure_raises_recoverable_error_then_recovers() {
  Rig r;
  r.robot.setMuted(false, r.t);
  r.conv.submitText("hi", "en", r.t);
  r.gw.err = "http_503";
  r.gw.next = GatewayPoll::Failed;
  r.run(false, 1);
  TEST_ASSERT_EQUAL((int)RobotState::Error, (int)r.state());
  TEST_ASSERT_EQUAL_STRING("http_503", r.robot.view(r.t).errorCode);
  r.run(false, 320);  // errorRecoverMs default 6 s
  TEST_ASSERT_EQUAL((int)RobotState::Idle, (int)r.state());
}

void test_auth_failure_is_sticky() {
  Rig r;
  r.robot.setMuted(false, r.t);
  r.conv.submitText("hi", "en", r.t);
  r.gw.err = "auth_rejected";
  r.gw.recoverable = false;
  r.gw.next = GatewayPoll::Failed;
  r.run(false, 400);
  TEST_ASSERT_EQUAL((int)RobotState::Error, (int)r.state());
}

void test_gateway_silence_times_out_and_cancels() {
  Rig r;
  r.robot.setMuted(false, r.t);
  r.conv.submitText("hi", "en", r.t);
  r.run(false, 1300);  // 26 s > 25 s processing deadline
  TEST_ASSERT_EQUAL(1, r.gw.cancels);
  TEST_ASSERT_TRUE(r.p.saw("gateway_timeout"));
}

void test_offline_speech_is_not_sent_and_says_so() {
  Rig r;
  r.robot.setMuted(false, r.t);
  r.gw.isReady = false;
  r.run(false, 50);
  r.run(true, 50);
  r.run(false, 45);
  TEST_ASSERT_EQUAL(0, r.gw.submitted);
  TEST_ASSERT_TRUE(r.p.saw("\"stage\":\"not_sent\""));
}

void test_mute_mid_utterance_drops_capture() {
  Rig r;
  r.robot.setMuted(false, r.t);
  r.run(false, 50);
  r.run(true, 30);
  TEST_ASSERT_EQUAL((int)Conversation::Phase::Capturing, (int)r.conv.phase());
  r.robot.onMuteButton(r.t);
  r.run(true, 5);
  TEST_ASSERT_EQUAL((int)Conversation::Phase::Waiting, (int)r.conv.phase());
  r.run(false, 60);
  TEST_ASSERT_EQUAL(0, r.gw.submitted);
}

void test_reply_with_content_shows_learning_card() {
  Rig r;
  r.robot.setMuted(false, r.t);
  r.conv.submitText("letter b", "en", r.t);
  r.gw.reply.contentKind = ContentKind::Letter;
  snprintf(r.gw.reply.contentValue, sizeof(r.gw.reply.contentValue), "B");
  r.gw.reply.hasAudio = false;  // subtitles only
  r.gw.next = GatewayPoll::Reply;
  r.run(false, 160);  // 2.5 s subtitle hold + tail
  TEST_ASSERT_EQUAL((int)RobotState::Learning, (int)r.state());
  TEST_ASSERT_EQUAL_STRING("B", r.robot.view(r.t).contentValue);
}

void test_text_only_reply_happy_then_paced_reading_then_listens() {
  Rig r;
  r.robot.setMuted(false, r.t);
  r.conv.submitText("why is the sky blue", "en", r.t);
  const char* answer = "Sunlight has many colors. The air scatters blue light the most, so the sky looks blue to us.";
  snprintf(r.gw.reply.longText, sizeof(r.gw.reply.longText), "%s", answer);
  r.gw.reply.hasAudio = false;  // robot without a speaker
  r.gw.next = GatewayPoll::Reply;
  r.run(false, 1);
  const ViewModel& v = r.robot.view(r.t);
  TEST_ASSERT_EQUAL((int)RobotState::Speaking, (int)v.state);
  TEST_ASSERT_EQUAL((int)Expression::Happy, (int)v.expression);  // happy face first
  TEST_ASSERT_EQUAL_STRING(answer, v.readingText);
  TEST_ASSERT_TRUE(v.readingSince > r.t);                          // text starts after the intro
  uint32_t hold = 1200 + readingTimeMs(answer, false);
  r.run(false, (int)(hold / 20) - 10);
  TEST_ASSERT_EQUAL((int)RobotState::Speaking, (int)r.state());    // still being read
  r.run(false, 40);
  TEST_ASSERT_EQUAL((int)RobotState::Listening, (int)r.state());   // then ready for the next question
  TEST_ASSERT_EQUAL_STRING("", r.robot.view(r.t).readingText);
  TEST_ASSERT_EQUAL(0, r.audio.stops);                              // never touched audio output
}

void test_missing_audio_does_not_hang() {
  Rig r;
  r.robot.setMuted(false, r.t);
  r.conv.submitText("hi", "en", r.t);
  r.gw.reply.hasAudio = true;  // promised audio never starts
  r.gw.next = GatewayPoll::Reply;
  r.run(false, 120);
  TEST_ASSERT_TRUE(r.p.saw("audio_missing"));
  TEST_ASSERT_NOT_EQUAL((int)RobotState::Speaking, (int)r.state());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_muted_robot_never_listens_or_submits);
  RUN_TEST(test_full_turn_listen_process_speak_and_rearm);
  RUN_TEST(test_sound_during_echo_tail_is_ignored);
  RUN_TEST(test_gateway_failure_raises_recoverable_error_then_recovers);
  RUN_TEST(test_auth_failure_is_sticky);
  RUN_TEST(test_gateway_silence_times_out_and_cancels);
  RUN_TEST(test_offline_speech_is_not_sent_and_says_so);
  RUN_TEST(test_mute_mid_utterance_drops_capture);
  RUN_TEST(test_reply_with_content_shows_learning_card);
  RUN_TEST(test_text_only_reply_happy_then_paced_reading_then_listens);
  RUN_TEST(test_missing_audio_does_not_hang);
  return UNITY_END();
}
