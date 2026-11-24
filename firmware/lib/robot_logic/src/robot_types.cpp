#include "robot_types.h"

#include <string.h>
#include <strings.h>

namespace robot {

namespace {
const char* const kStateNames[] = {"STARTUP",  "IDLE",     "LISTENING", "PROCESSING",
                                   "SPEAKING", "LEARNING", "MUTED",     "ERROR"};
const char* const kExprNames[] = {"idle",    "happy",   "listening", "thinking", "speaking", "blink",   "sleeping",
                                  "surprised", "sad",   "muted",     "error",    "excited",  "curious", "confused",
                                  "worried", "playful", "proud",     "offline",  "sleepy",   "love",
                                  "starstruck", "dizzy", "laughing", "crying", "annoyed", "shy",   "scared",
                                  "mischievous", "bored", "cool",  "boot"};
const char* const kKindNames[] = {"none", "letter", "number", "math", "word"};

static_assert(sizeof(kStateNames) / sizeof(kStateNames[0]) == (size_t)RobotState::kCount, "state names");
static_assert(sizeof(kExprNames) / sizeof(kExprNames[0]) == (size_t)Expression::kCount, "expr names");
static_assert(sizeof(kKindNames) / sizeof(kKindNames[0]) == (size_t)ContentKind::kCount, "kind names");

template <typename E, size_t N>
bool parseName(const char* s, const char* const (&names)[N], E& out) {
  if (!s) return false;
  for (size_t i = 0; i < N; i++) {
    if (strcasecmp(s, names[i]) == 0) {
      out = static_cast<E>(i);
      return true;
    }
  }
  return false;
}
}  // namespace

const char* toString(RobotState s) {
  return (size_t)s < (size_t)RobotState::kCount ? kStateNames[(size_t)s] : "?";
}
const char* toString(Expression e) {
  return (size_t)e < (size_t)Expression::kCount ? kExprNames[(size_t)e] : "?";
}
const char* toString(ContentKind k) {
  return (size_t)k < (size_t)ContentKind::kCount ? kKindNames[(size_t)k] : "?";
}

bool parseState(const char* s, RobotState& out) {
  // "THINKING" is accepted as a friendly alias for PROCESSING.
  if (s && strcasecmp(s, "THINKING") == 0) {
    out = RobotState::Processing;
    return true;
  }
  return parseName(s, kStateNames, out);
}
bool parseExpression(const char* s, Expression& out) {
  // Boot is internal-only; it cannot be requested.
  Expression e;
  if (!parseName(s, kExprNames, e) || e == Expression::Boot) return false;
  out = e;
  return true;
}
bool parseContentKind(const char* s, ContentKind& out) {
  ContentKind k;
  if (!parseName(s, kKindNames, k) || k == ContentKind::None) return false;
  out = k;
  return true;
}

Expression defaultExpression(RobotState s) {
  switch (s) {
    case RobotState::Startup: return Expression::Boot;
    case RobotState::Idle: return Expression::Idle;
    case RobotState::Listening: return Expression::Listening;
    case RobotState::Processing: return Expression::Thinking;
    case RobotState::Speaking: return Expression::Speaking;
    case RobotState::Learning: return Expression::Happy;
    case RobotState::Muted: return Expression::Muted;
    case RobotState::Error: return Expression::Error;
    default: return Expression::Idle;
  }
}

}  // namespace robot
