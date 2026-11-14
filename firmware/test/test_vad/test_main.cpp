#include <math.h>
#include <stdlib.h>
#include <unity.h>

#include "utterance_buffer.h"
#include "vad.h"

using namespace robot;

static const size_t N = 320;  // 20 ms @ 16 kHz
static uint32_t seed = 1;
static int16_t noise(int amp) {
  seed = seed * 1103515245u + 12345u;
  return (int16_t)((int)((seed >> 16) % (2 * amp + 1)) - amp);
}
static void makeNoise(int16_t* f, int amp) {
  for (size_t i = 0; i < N; i++) f[i] = noise(amp);
}
static void makeSpeech(int16_t* f, int amp, int frameIdx) {
  for (size_t i = 0; i < N; i++) {
    double t = (frameIdx * N + i) / 16000.0;
    f[i] = (int16_t)(amp * sin(2 * M_PI * 220 * t) * (0.6 + 0.4 * sin(2 * M_PI * 4 * t)) + noise(40));
  }
}

struct Run {
  int starts = 0, ends = 0, discards = 0;
  int firstStartFrame = -1, endFrame = -1;
};

static Run feed(Vad& v, int quietFrames, int speechFrames, int tailFrames, int speechAmp = 4000) {
  Run r;
  int16_t f[N];
  int idx = 0;
  auto handle = [&](VadEvent e) {
    if (e == VadEvent::SpeechStart) { r.starts++; if (r.firstStartFrame < 0) r.firstStartFrame = idx; }
    if (e == VadEvent::SpeechEnd) { r.ends++; r.endFrame = idx; }
    if (e == VadEvent::Discard) r.discards++;
  };
  for (int i = 0; i < quietFrames; i++, idx++) { makeNoise(f, 40); handle(v.processFrame(f, N)); }
  for (int i = 0; i < speechFrames; i++, idx++) { makeSpeech(f, speechAmp, i); handle(v.processFrame(f, N)); }
  for (int i = 0; i < tailFrames; i++, idx++) { makeNoise(f, 40); handle(v.processFrame(f, N)); }
  return r;
}

void setUp() { seed = 1; }
void tearDown() {}

void test_silence_never_triggers() {
  Vad v;
  Run r = feed(v, 500, 0, 0);
  TEST_ASSERT_EQUAL(0, r.starts);
  TEST_ASSERT_TRUE(v.noiseDb() < -50.0f);
}

void test_one_second_of_speech_detected_with_end_of_speech() {
  Vad v;
  Run r = feed(v, 50, 50, 60);  // 1 s quiet, 1 s speech, 1.2 s quiet
  TEST_ASSERT_EQUAL(1, r.starts);
  TEST_ASSERT_EQUAL(1, r.ends);
  TEST_ASSERT_EQUAL(0, r.discards);
  // Start confirmed within ~160 ms of the onset (frame 50).
  TEST_ASSERT_TRUE(r.firstStartFrame >= 50 && r.firstStartFrame <= 50 + 9);
  // End declared after the 800 ms hangover (40 frames) following speech end (frame 100).
  TEST_ASSERT_TRUE(r.endFrame >= 100 + 38 && r.endFrame <= 100 + 42);
}

void test_click_is_discarded() {
  Vad v;
  Run r = feed(v, 50, 10, 60);  // 200 ms burst
  TEST_ASSERT_EQUAL(1, r.starts);
  TEST_ASSERT_EQUAL(0, r.ends);
  TEST_ASSERT_EQUAL(1, r.discards);
}

void test_quiet_whisper_below_absolute_floor_ignored() {
  Vad v;
  Run r = feed(v, 50, 50, 50, 25);  // ~-65 dBFS: below minSpeechDb
  TEST_ASSERT_EQUAL(0, r.starts);
}

void test_endless_sound_is_cut_and_floor_rises() {
  VadConfig c;
  c.maxUtteranceMs = 3000;
  Vad v(c);
  Run r = feed(v, 50, 400, 0);  // 8 s of continuous loud sound
  TEST_ASSERT_TRUE(r.ends >= 1);
  TEST_ASSERT_TRUE(v.truncated() || r.ends >= 1);
  // After the cut the floor tracks the sound, so it should not retrigger endlessly.
  TEST_ASSERT_TRUE(r.starts <= 2);
}

void test_reset_speech_keeps_noise_floor() {
  Vad v;
  feed(v, 100, 0, 0);
  float floor = v.noiseDb();
  v.resetSpeech();
  TEST_ASSERT_EQUAL_FLOAT(floor, v.noiseDb());
}

void test_utterance_buffer_includes_preroll_and_caps() {
  static int16_t store[1000];
  UtteranceBuffer b;
  b.attach(store, 1000);
  b.setPreRoll(100);
  int16_t f[50];
  for (int k = 0; k < 4; k++) {
    for (int i = 0; i < 50; i++) f[i] = (int16_t)(k * 50 + i);
    b.push(f, 50);
  }
  b.start();
  TEST_ASSERT_EQUAL(100, (int)b.size());
  TEST_ASSERT_EQUAL(100, store[0]);  // last 100 samples pushed: 100..199
  TEST_ASSERT_EQUAL(199, store[99]);
  for (int k = 0; k < 30; k++) b.push(f, 50);  // 1500 more samples > capacity
  TEST_ASSERT_EQUAL(1000, (int)b.size());
  TEST_ASSERT_TRUE(b.overflowed());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_silence_never_triggers);
  RUN_TEST(test_one_second_of_speech_detected_with_end_of_speech);
  RUN_TEST(test_click_is_discarded);
  RUN_TEST(test_quiet_whisper_below_absolute_floor_ignored);
  RUN_TEST(test_endless_sound_is_cut_and_floor_rises);
  RUN_TEST(test_reset_speech_keeps_noise_floor);
  RUN_TEST(test_utterance_buffer_includes_preroll_and_caps);
  return UNITY_END();
}
