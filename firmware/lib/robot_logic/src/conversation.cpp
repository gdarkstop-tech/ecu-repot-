#include "conversation.h"

#include <stdio.h>
#include <string.h>

#include "utf8.h"

namespace robot {

const char* toString(Conversation::Phase p) {
  switch (p) {
    case Conversation::Phase::Waiting: return "waiting";
    case Conversation::Phase::Capturing: return "capturing";
    case Conversation::Phase::Processing: return "processing";
    case Conversation::Phase::Speaking: return "speaking";
    case Conversation::Phase::Tail: return "tail";
  }
  return "?";
}

Conversation::Conversation(Robot& robot, Vad& vad, UtteranceBuffer& buffer, IGateway& gateway,
                           IAudioOutput& audio, const ConversationConfig& cfg)
    : robot_(robot), vad_(vad), buf_(buffer), gw_(gateway), audio_(audio), cfg_(cfg) {}

void Conversation::emitTurnEvent(const char* stage, const char* detail) {
  char line[200];
  if (detail) {
    snprintf(line, sizeof(line), "{\"evt\":\"turn\",\"stage\":\"%s\",\"n\":%lu,\"detail\":\"%s\"}", stage,
             (unsigned long)turns_, detail);
  } else {
    snprintf(line, sizeof(line), "{\"evt\":\"turn\",\"stage\":\"%s\",\"n\":%lu}", stage, (unsigned long)turns_);
  }
  robot_.emitRaw(line);
}

bool Conversation::micWanted(uint32_t) const {
  if (robot_.stateMachine().muted()) return false;
  if (phase_ != Phase::Waiting && phase_ != Phase::Capturing) return false;
  RobotState s = robot_.stateMachine().state();
  return s == RobotState::Idle || s == RobotState::Listening || s == RobotState::Learning;
}

void Conversation::onMicFrame(const int16_t* pcm, size_t n, uint32_t now) {
  if (!micWanted(now)) {
    // Deaf: do not let our own speaker output (or anything during processing) into the VAD.
    if (phase_ == Phase::Capturing) {
      // Muted (or forced elsewhere) mid-utterance: drop it.
      buf_.stop();
      buf_.clear();
      phase_ = Phase::Waiting;
      emitTurnEvent("dropped", "mic_disabled");
    }
    robot_.setMicActivity(false, 0);
    return;
  }

  buf_.push(pcm, n);
  VadEvent ev = vad_.processFrame(pcm, n);
  robot_.setMicActivity(vad_.inSpeech(), vad_.inSpeech() ? vad_.uiLevel() : 0);

  switch (ev) {
    case VadEvent::SpeechStart:
      buf_.start();
      phase_ = Phase::Capturing;
      phaseSince_ = now;
      robot_.stateMachine().noteActivity(now);
      // Hold LISTENING while the child is talking (no timeout mid-sentence).
      robot_.requestState(RobotState::Listening, now, UINT32_MAX);
      emitTurnEvent("speech_start");
      break;
    case VadEvent::SpeechEnd:
      buf_.stop();
      onSpeechEnd(now);
      break;
    case VadEvent::Discard:
      buf_.stop();
      buf_.clear();
      phase_ = Phase::Waiting;
      emitTurnEvent("discarded", "too_short");
      robot_.requestState(RobotState::Listening, now, cfg_.followUpMs);
      break;
    default:
      break;
  }
}

void Conversation::onSpeechEnd(uint32_t now) {
  turns_++;
  if (!gw_.ready() || !gw_.startUtterance(buf_.data(), buf_.size(), vad_.config().sampleRate)) {
    // No network: the offline activities take over; say so on screen instead of pretending.
    phase_ = Phase::Waiting;
    buf_.clear();
    failures_++;
    snprintf(lastError_, sizeof(lastError_), "offline");
    emitTurnEvent("not_sent", "offline");
    if (offlineFn_) {
      offlineFn_(now, offlineCtx_);
    } else {
      robot_.setExpressionOverride(Expression::Sad, now, 1500);
      robot_.requestState(RobotState::Idle, now);
    }
    return;
  }
  phase_ = Phase::Processing;
  phaseSince_ = now;
  char detail[32];
  snprintf(detail, sizeof(detail), "%lu_samples", (unsigned long)buf_.size());
  emitTurnEvent("submitted", detail);
  robot_.requestState(RobotState::Processing, now);
}

bool Conversation::submitText(const char* text, const char* lang, uint32_t now) {
  if (phase_ == Phase::Processing || phase_ == Phase::Speaking) return false;
  if (!gw_.ready() || !gw_.startText(text, lang)) return false;
  turns_++;
  buf_.stop();
  buf_.clear();
  vad_.resetSpeech();
  phase_ = Phase::Processing;
  phaseSince_ = now;
  emitTurnEvent("submitted", "text");
  robot_.requestState(RobotState::Processing, now);
  return true;
}

void Conversation::tick(uint32_t now) {
  RobotState st = robot_.stateMachine().state();
  switch (phase_) {
    case Phase::Waiting:
    case Phase::Capturing:
      return;

    case Phase::Processing: {
      if (st != RobotState::Processing) {
        // Timed out (state machine deadline) or interrupted: abandon the request.
        gw_.cancel();
        failures_++;
        snprintf(lastError_, sizeof(lastError_), "gateway_timeout");
        emitTurnEvent("failed", "gateway_timeout");
        phase_ = Phase::Waiting;
        buf_.clear();
        if (st != RobotState::Error) robot_.raiseError("gateway_timeout", true, now);
        return;
      }
      char err[48] = {0};
      bool recoverable = true;
      GatewayPoll p = gw_.poll(reply_, err, sizeof(err), recoverable);
      if (p == GatewayPoll::Busy || p == GatewayPoll::Idle) {
        return;
      }
      buf_.clear();
      if (p == GatewayPoll::Failed) {
        failures_++;
        snprintf(lastError_, sizeof(lastError_), "%s", err[0] ? err : "gateway_error");
        emitTurnEvent("failed", lastError_);
        phase_ = Phase::Waiting;
        robot_.raiseError(lastError_, recoverable, now);
        return;
      }
      // Reply
      if (reply_.ignore) {
        emitTurnEvent("ignored");
        rearm(now);
        return;
      }
      emitTurnEvent("reply");
      phase_ = Phase::Speaking;
      phaseSince_ = now;
      audioSeen_ = false;
      if (!reply_.hasAudio && reply_.longText[0]) {
        // Text-only robot: a happy face, then the whole answer scrolls, paced for reading.
        uint32_t readMs = readingTimeMs(reply_.longText, reply_.longRtl);
        textHoldMs_ = cfg_.textIntroMs + readMs;
        robot_.requestState(RobotState::Speaking, now, textHoldMs_ + 3000);
        robot_.setExpressionOverride(Expression::Happy, now, cfg_.textIntroMs);
        robot_.showReading(reply_.longText, reply_.longRtl, now + cfg_.textIntroMs, readMs);
      } else {
        textHoldMs_ = 2500;  // subtitle-only reply: let it be read briefly
        robot_.requestState(RobotState::Speaking, now);
        if (reply_.oledText[0]) robot_.setMarquee(reply_.oledText, reply_.rtl, now);
      }
      return;
    }

    case Phase::Speaking: {
      if (st != RobotState::Speaking) {
        // Safety deadline or external interruption: silence the speaker.
        audio_.stop();
        finishSpeaking(now);
        return;
      }
      bool active = audio_.active();
      if (active) audioSeen_ = true;
      robot_.setMicActivity(false, active ? audio_.level() : 0);
      uint32_t elapsed = now - phaseSince_;
      bool noAudioExpected = !reply_.hasAudio && elapsed >= textHoldMs_;  // let the text be read
      bool audioNeverStarted = reply_.hasAudio && !audioSeen_ && elapsed >= cfg_.playbackStartGraceMs;
      if ((audioSeen_ && !active) || noAudioExpected || audioNeverStarted) {
        if (audioNeverStarted) emitTurnEvent("audio_missing");
        finishSpeaking(now);
      }
      return;
    }

    case Phase::Tail:
      if ((uint32_t)(now - phaseSince_) >= cfg_.echoTailMs) rearm(now);
      return;
  }
}

void Conversation::finishSpeaking(uint32_t now) {
  phase_ = Phase::Tail;
  phaseSince_ = now;
  robot_.setMicActivity(false, 0);
  emitTurnEvent("spoken");
}

void Conversation::rearm(uint32_t now) {
  phase_ = Phase::Waiting;
  vad_.resetSpeech();
  buf_.clear();
  if (robot_.stateMachine().state() == RobotState::Error) return;
  if (reply_.contentKind != ContentKind::None && !reply_.ignore) {
    robot_.showContent(reply_.contentKind, reply_.contentValue, reply_.contentCaption, reply_.rtl, now,
                       cfg_.learningHoldMs);
  } else if (!robot_.stateMachine().muted()) {
    robot_.requestState(RobotState::Listening, now, cfg_.followUpMs);
  } else {
    robot_.requestState(RobotState::Idle, now);
  }
  if (!reply_.ignore && reply_.emotion != Expression::Speaking) {
    robot_.setExpressionOverride(reply_.emotion, now, cfg_.emotionMs);
  }
  reply_ = GatewayReply();
}

}  // namespace robot
