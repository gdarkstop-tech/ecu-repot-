// Standalone voice conversation loop:
//
//   (mic armed) --speech--> LISTENING --end of speech--> PROCESSING --reply--> SPEAKING
//        ^                                                                       |
//        +------ follow-up window (LISTENING) <---- echo-guard tail <------------+
//
// Echo prevention (the robot must not hear itself): the microphone path is ignored from the
// moment an utterance is submitted until echoTailMs AFTER playback has fully drained, and the
// VAD's speech state is reset before re-arming. The amplifier is shut down when idle (HW layer).
//
// Network and audio output are behind small interfaces so this file runs unchanged in unit
// tests with fakes, and on the ESP32 with the real gateway client / I2S output.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "debug_protocol.h"
#include "robot.h"
#include "utterance_buffer.h"
#include "vad.h"

namespace robot {

struct GatewayReply {
  bool ignore = false;  // nothing worth answering (noise / empty transcript)
  char oledText[kMaxTextBytes + 1] = {0};
  bool rtl = false;
  ContentKind contentKind = ContentKind::None;
  char contentValue[kMaxValueBytes + 1] = {0};
  char contentCaption[kMaxTextBytes + 1] = {0};
  char longText[kMaxLongTextBytes + 1] = {0};  // full answer for text-only robots
  bool longRtl = false;
  Expression emotion = Expression::Happy;
  bool hasAudio = false;
  char lang[8] = {0};
};

enum class GatewayPoll : uint8_t { Idle = 0, Busy, Reply, Failed };

class IGateway {
 public:
  virtual ~IGateway() = default;
  // Wi-Fi connected and a gateway URL + device token are configured.
  virtual bool ready() const = 0;
  // Non-blocking: the request runs on the network task. The PCM buffer must stay valid
  // until poll() returns Reply or Failed.
  virtual bool startUtterance(const int16_t* pcm, size_t samples, uint32_t sampleRate) = 0;
  virtual bool startText(const char* text, const char* lang) = 0;
  // On Reply, the client has already begun streaming the reply audio to the speaker.
  virtual GatewayPoll poll(GatewayReply& reply, char* err, size_t errLen, bool& recoverable) = 0;
  virtual void cancel() = 0;
};

class IAudioOutput {
 public:
  virtual ~IAudioOutput() = default;
  virtual bool active() const = 0;    // audio still arriving or still playing
  virtual uint8_t level() const = 0;  // 0..100 playback envelope (drives the mouth)
  virtual void stop() = 0;
};

struct ConversationConfig {
  uint32_t echoTailMs = 400;        // mic stays deaf this long after playback drains
  uint32_t followUpMs = 8000;       // LISTENING window after the robot answers
  uint32_t playbackStartGraceMs = 1500;  // time allowed for reply audio to begin
  uint32_t learningHoldMs = 20000;  // how long reply content (e.g. a letter) stays up
  uint32_t emotionMs = 1800;        // reply emotion shown after speaking
  uint32_t textIntroMs = 1200;      // happy face before the answer text starts scrolling
};

class Conversation {
 public:
  enum class Phase : uint8_t { Waiting = 0, Capturing, Processing, Speaking, Tail };

  Conversation(Robot& robot, Vad& vad, UtteranceBuffer& buffer, IGateway& gateway, IAudioOutput& audio,
               const ConversationConfig& cfg = ConversationConfig());

  // Feed every captured 20 ms frame (16 kHz mono). Frames are ignored while the mic must be deaf.
  void onMicFrame(const int16_t* pcm, size_t n, uint32_t now);
  void tick(uint32_t now);

  // Called at end of speech when the gateway is unreachable (e.g. advance offline cards).
  // Without a handler the robot shows a brief sad face and stays put.
  using OfflineFn = void (*)(uint32_t now, void* ctx);
  void setOfflineHandler(OfflineFn fn, void* ctx) {
    offlineFn_ = fn;
    offlineCtx_ = ctx;
  }

  // Debug/test path: behave as if `text` had been spoken.
  bool submitText(const char* text, const char* lang, uint32_t now);

  bool micWanted(uint32_t now) const;
  Phase phase() const { return phase_; }
  uint32_t turns() const { return turns_; }
  uint32_t failures() const { return failures_; }
  const char* lastError() const { return lastError_; }

 private:
  void onSpeechEnd(uint32_t now);
  void finishSpeaking(uint32_t now);
  void rearm(uint32_t now);
  void emitTurnEvent(const char* stage, const char* detail = nullptr);

  Robot& robot_;
  Vad& vad_;
  UtteranceBuffer& buf_;
  IGateway& gw_;
  IAudioOutput& audio_;
  ConversationConfig cfg_;

  Phase phase_ = Phase::Waiting;
  uint32_t phaseSince_ = 0;
  bool audioSeen_ = false;
  uint32_t textHoldMs_ = 2500;  // how long SPEAKING lasts when there is no reply audio
  GatewayReply reply_;
  OfflineFn offlineFn_ = nullptr;
  void* offlineCtx_ = nullptr;
  uint32_t turns_ = 0;
  uint32_t failures_ = 0;
  char lastError_[48] = {0};
};

const char* toString(Conversation::Phase p);

}  // namespace robot
