// Core robot vocabulary shared by firmware, simulator, and tests.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace robot {

// Conversation / behaviour states. The robot drives these itself in standalone mode;
// the USB debug console may also request them for testing.
enum class RobotState : uint8_t {
  Startup = 0,
  Idle,        // unmuted, microphone armed, waiting for speech
  Listening,   // capturing (or awaiting) the user's speech
  Processing,  // utterance sent to the AI gateway, waiting for the reply
  Speaking,    // playing the reply audio
  Learning,    // showing educational content (letter, number, sum, word)
  Muted,       // microphone muted (default at power-on)
  Error,
  kCount
};

// Visual expressions of the face (BMO-inspired style, see face_model.h). Blink is a one-shot
// animation, not a mood. Names are part of the USB protocol and the gateway contract.
enum class Expression : uint8_t {
  Idle = 0,   // neutral: dot eyes, small smile
  Happy,      // open "D" smile
  Listening,
  Thinking,
  Speaking,
  Blink,
  Sleeping,   // eyes closed, Zz, screen dims
  Surprised,
  Sad,
  Muted,
  Error,
  Excited,    // very happy: ^ ^ eyes, big grin, blush, sparkles
  Curious,
  Confused,
  Worried,
  Playful,    // wink + tongue
  Proud,      // smug: lidded eyes, smirk
  Offline,
  Sleepy,     // drowsy, half-closed eyes
  Boot,
  kCount
};

// What a Learning screen shows.
enum class ContentKind : uint8_t { None = 0, Letter, Number, Math, Word, kCount };

const char* toString(RobotState s);
const char* toString(Expression e);
const char* toString(ContentKind k);
bool parseState(const char* s, RobotState& out);
bool parseExpression(const char* s, Expression& out);
bool parseContentKind(const char* s, ContentKind& out);

// The expression a state shows when nothing overrides it.
Expression defaultExpression(RobotState s);

}  // namespace robot
