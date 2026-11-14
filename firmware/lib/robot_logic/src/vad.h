// Energy-based voice activity detector with an adaptive noise floor.
//
// Deliberately simple and deterministic: it runs on 20 ms frames of 16 kHz mono PCM, costs a
// few microseconds per frame on the ESP32-S3, and is unit-tested with synthetic signals.
//
//   start of speech : frame energy > noise + startMarginDb (and > minSpeechDb) for minSpeechMs
//   end of speech   : energy < noise + stopMarginDb for hangoverMs   (end-of-speech detection)
//   safety caps     : utterances shorter than minUtteranceMs are discarded (clicks, bumps);
//                     longer than maxUtteranceMs are cut and sent anyway.
//
// The noise floor only adapts while NOT in speech, so a child talking does not raise it.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace robot {

struct VadConfig {
  uint32_t sampleRate = 16000;
  uint32_t frameMs = 20;
  float startMarginDb = 10.0f;
  float stopMarginDb = 6.0f;
  float minSpeechDb = -58.0f;     // absolute dBFS floor; quieter is never speech
  uint32_t minSpeechMs = 160;     // consecutive loud time needed to declare speech
  uint32_t hangoverMs = 800;      // trailing silence that ends an utterance
  uint32_t minUtteranceMs = 400;  // shorter utterances are discarded
  uint32_t maxUtteranceMs = 12000;
  float noiseAdapt = 0.05f;       // EMA weight per quiet frame
  float initialNoiseDb = -70.0f;
};

enum class VadEvent : uint8_t {
  None = 0,
  SpeechStart,  // caller should begin capturing (including pre-roll)
  SpeechEnd,    // valid utterance complete
  Discard,      // too short: drop the capture
};

class Vad {
 public:
  explicit Vad(const VadConfig& cfg = VadConfig());
  void reset();
  // Forget any speech in progress but keep the learned noise floor (used when re-arming the
  // microphone after the robot has spoken).
  void resetSpeech() {
    inSpeech_ = false;
    truncated_ = false;
    loudMs_ = quietMs_ = speechMs_ = 0;
  }
  VadEvent processFrame(const int16_t* pcm, size_t n);

  bool inSpeech() const { return inSpeech_; }
  bool truncated() const { return truncated_; }  // last SpeechEnd hit maxUtteranceMs
  float levelDb() const { return levelDb_; }
  float noiseDb() const { return noiseDb_; }
  uint32_t speechMs() const { return speechMs_; }
  // 0..100 level for the OLED listening bars (relative to the noise floor).
  uint8_t uiLevel() const;
  const VadConfig& config() const { return cfg_; }

  static float frameDb(const int16_t* pcm, size_t n);

 private:
  VadConfig cfg_;
  bool inSpeech_ = false;
  bool truncated_ = false;
  float levelDb_ = -100.0f;
  float noiseDb_ = -70.0f;
  uint32_t loudMs_ = 0;
  uint32_t quietMs_ = 0;
  uint32_t speechMs_ = 0;
  uint32_t framesSeen_ = 0;
};

const char* toString(VadEvent e);

}  // namespace robot
