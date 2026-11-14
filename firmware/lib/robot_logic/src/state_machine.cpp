#include "state_machine.h"

#include <string.h>

namespace robot {

const char* toString(TransitionResult r) {
  switch (r) {
    case TransitionResult::Ok: return "ok";
    case TransitionResult::Unchanged: return "unchanged";
    case TransitionResult::RejectedStartup: return "starting";
    case TransitionResult::RejectedMuted: return "mic_muted";
    case TransitionResult::RejectedInvalid: return "invalid_transition";
  }
  return "?";
}

const char* toString(TransitionReason r) {
  switch (r) {
    case TransitionReason::Request: return "request";
    case TransitionReason::Timeout: return "timeout";
    case TransitionReason::Mute: return "mute";
    case TransitionReason::Unmute: return "unmute";
    case TransitionReason::StartupDone: return "startup_done";
    case TransitionReason::Fault: return "fault";
  }
  return "?";
}

// Wrap-safe "a is at or after b" for millis() style counters.
static inline bool reached(uint32_t now, uint32_t when) { return (int32_t)(now - when) >= 0; }

StateMachine::StateMachine(const StateConfig& cfg) : cfg_(cfg) {}

void StateMachine::begin(uint32_t now) {
  state_ = RobotState::Startup;
  muted_ = true;  // requirement: microphone muted at startup
  since_ = now;
  lastActivity_ = now;
  transitions_ = 0;
  errorCode_[0] = 0;
  deadline_ = now + cfg_.startupMs;
  hasDeadline_ = true;
}

uint32_t StateMachine::defaultTimeout(RobotState s) const {
  switch (s) {
    case RobotState::Processing: return cfg_.processingTimeoutMs;
    case RobotState::Speaking: return cfg_.speakingTimeoutMs;
    default: return 0;  // no deadline
  }
}

void StateMachine::enter(RobotState to, uint32_t now, uint32_t timeoutMs, TransitionReason why) {
  RobotState from = state_;
  state_ = to;
  since_ = now;
  lastReason_ = why;
  transitions_++;
  if (timeoutMs == 0) timeoutMs = defaultTimeout(to);
  hasDeadline_ = timeoutMs != 0 && timeoutMs != UINT32_MAX;
  deadline_ = hasDeadline_ ? now + timeoutMs : 0;
  if (to != RobotState::Error) errorCode_[0] = 0;
  if (listener_) listener_(from, to, why, listenerCtx_);
}

TransitionResult StateMachine::request(RobotState target, uint32_t now, uint32_t timeoutMs) {
  if (target == RobotState::Startup || target == RobotState::Muted || target >= RobotState::kCount) {
    return TransitionResult::RejectedInvalid;
  }
  if (state_ == RobotState::Startup && target != RobotState::Error) {
    return TransitionResult::RejectedStartup;
  }
  if (target == RobotState::Idle) target = restState();
  if (target == RobotState::Listening && muted_) return TransitionResult::RejectedMuted;

  if (target == state_) {
    // Refresh the deadline so repeated requests keep e.g. PROCESSING alive.
    if (timeoutMs == 0) timeoutMs = defaultTimeout(target);
    hasDeadline_ = timeoutMs != 0 && timeoutMs != UINT32_MAX;
    deadline_ = hasDeadline_ ? now + timeoutMs : 0;
    return TransitionResult::Unchanged;
  }
  enter(target, now, timeoutMs, TransitionReason::Request);
  return TransitionResult::Ok;
}

void StateMachine::extendDeadline(uint32_t now, uint32_t ms) {
  if (!hasDeadline_) return;
  uint32_t candidate = now + ms;
  if ((int32_t)(candidate - deadline_) > 0) deadline_ = candidate;
}

void StateMachine::setMuted(bool muted, uint32_t now) {
  if (muted == muted_) return;
  muted_ = muted;
  lastActivity_ = now;
  if (state_ == RobotState::Startup) return;  // startup ends in the (new) rest state
  if (muted) {
    // Muting stops any capture immediately; output states continue.
    if (state_ == RobotState::Idle || state_ == RobotState::Listening) {
      enter(RobotState::Muted, now, UINT32_MAX, TransitionReason::Mute);
    }
  } else if (state_ == RobotState::Muted) {
    enter(RobotState::Idle, now, UINT32_MAX, TransitionReason::Unmute);
  }
}

void StateMachine::raiseError(const char* code, bool recoverable, uint32_t now) {
  errorRecoverable_ = recoverable;
  // Set the code first so the transition listener can report it.
  strncpy(errorCode_, code ? code : "unknown", sizeof(errorCode_) - 1);
  errorCode_[sizeof(errorCode_) - 1] = 0;
  enter(RobotState::Error, now, recoverable ? cfg_.errorRecoverMs : UINT32_MAX, TransitionReason::Fault);
}

void StateMachine::tick(uint32_t now) {
  if (!hasDeadline_ || !reached(now, deadline_)) return;
  if (state_ == RobotState::Startup) {
    enter(restState(), now, UINT32_MAX, TransitionReason::StartupDone);
    return;
  }
  enter(restState(), now, UINT32_MAX, TransitionReason::Timeout);
}

bool StateMachine::sleeping(uint32_t now) const {
  if (state_ != RobotState::Idle && state_ != RobotState::Muted) return false;
  return (uint32_t)(now - lastActivity_) >= cfg_.sleepAfterMs;
}

}  // namespace robot
