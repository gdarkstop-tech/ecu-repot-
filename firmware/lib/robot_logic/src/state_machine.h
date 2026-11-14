// Deterministic robot state machine.
//
// Rules (enforced here, never by the AI model):
//   * STARTUP can only be left by its own timer (or an error). It cannot be re-entered.
//   * The microphone is muted at power-on, so STARTUP ends in MUTED.
//   * LISTENING is impossible while muted.
//   * IDLE is the "rest" request: it resolves to MUTED when the mic is muted.
//   * MUTED cannot be requested directly; use setMuted().
//   * Every state may carry a deadline. When it passes, tick() returns to the rest state.
//     This is what recovers the robot when the network or a peer stops answering.
#pragma once
#include <stdint.h>

#include "robot_types.h"

namespace robot {

struct StateConfig {
  uint32_t startupMs = 2200;
  uint32_t processingTimeoutMs = 25000;  // gateway must answer within this window
  uint32_t speakingTimeoutMs = 90000;    // safety cap if "playback done" never arrives
  uint32_t errorRecoverMs = 6000;        // recoverable errors clear themselves
  uint32_t sleepAfterMs = 180000;        // rest state shows the sleeping face after this
};

enum class TransitionResult : uint8_t {
  Ok = 0,
  Unchanged,        // already in that state (deadline refreshed)
  RejectedStartup,  // still booting
  RejectedMuted,    // LISTENING requested while muted
  RejectedInvalid,  // STARTUP/MUTED/unknown requested
};
const char* toString(TransitionResult r);

enum class TransitionReason : uint8_t { Request = 0, Timeout, Mute, Unmute, StartupDone, Fault };
const char* toString(TransitionReason r);

class StateMachine {
 public:
  using Listener = void (*)(RobotState from, RobotState to, TransitionReason why, void* ctx);

  explicit StateMachine(const StateConfig& cfg = StateConfig());

  void begin(uint32_t now);

  // Request a state. timeoutMs: 0 = use the state's default deadline, UINT32_MAX = no deadline.
  TransitionResult request(RobotState target, uint32_t now, uint32_t timeoutMs = 0);

  // Push the current state's deadline out (e.g. audio is still streaming in).
  void extendDeadline(uint32_t now, uint32_t ms);

  void setMuted(bool muted, uint32_t now);

  // Enter ERROR. Recoverable errors return to rest after errorRecoverMs.
  void raiseError(const char* code, bool recoverable, uint32_t now);

  // User-visible activity (speech, button, command). Resets the sleep timer.
  void noteActivity(uint32_t now) { lastActivity_ = now; }

  void tick(uint32_t now);

  RobotState state() const { return state_; }
  RobotState restState() const { return muted_ ? RobotState::Muted : RobotState::Idle; }
  bool muted() const { return muted_; }
  bool sleeping(uint32_t now) const;
  bool inStartup() const { return state_ == RobotState::Startup; }
  const char* errorCode() const { return errorCode_; }
  uint32_t stateSince() const { return since_; }
  uint32_t deadline() const { return deadline_; }
  uint32_t transitionCount() const { return transitions_; }
  TransitionReason lastReason() const { return lastReason_; }

  void setListener(Listener fn, void* ctx) {
    listener_ = fn;
    listenerCtx_ = ctx;
  }

 private:
  void enter(RobotState to, uint32_t now, uint32_t timeoutMs, TransitionReason why);
  uint32_t defaultTimeout(RobotState s) const;

  StateConfig cfg_;
  RobotState state_ = RobotState::Startup;
  bool muted_ = true;
  uint32_t since_ = 0;
  uint32_t deadline_ = 0;  // 0 = none
  bool hasDeadline_ = false;
  uint32_t lastActivity_ = 0;
  uint32_t transitions_ = 0;
  bool errorRecoverable_ = true;
  char errorCode_[32] = {0};
  TransitionReason lastReason_ = TransitionReason::Request;
  Listener listener_ = nullptr;
  void* listenerCtx_ = nullptr;
};

}  // namespace robot
