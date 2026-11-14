// USB debug / provisioning protocol: newline-delimited JSON, one object per line.
//
// This link is for development, testing and first-time setup only. The robot never
// needs it during normal operation. See docs/PROTOCOL.md for the full message list.
//
//   laptop -> robot   {"v":1,"id":7,"cmd":"face","expr":"happy","ms":1500}
//   robot  -> laptop  {"evt":"ack","id":7,"ok":true}
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "robot_types.h"

namespace robot {

constexpr int kProtocolVersion = 1;
constexpr size_t kMaxTextBytes = 160;   // UTF-8 bytes (~50 Arabic letters, ~160 Latin)
constexpr size_t kMaxValueBytes = 32;
constexpr size_t kMaxKeyBytes = 24;
constexpr uint32_t kMaxDurationMs = 600000;

enum class CmdType : uint8_t {
  Ping = 0,
  Hello,
  Status,
  State,
  Face,
  Text,
  Show,
  Say,
  Mute,
  Clear,
  Selftest,
  Config,     // set a setting (Wi-Fi, gateway URL, device token, language, age, volume)
  ConfigGet,  // read non-secret settings
  Reboot,
  I2cScan,
  MicTest,    // record N ms from the INMP441 and report levels
  Tone,       // play a sine tone through the MAX98357A
  WifiScan,
  TurnText,   // send text to the AI gateway as if it had been spoken (tests network+TTS+speaker)
  Loopback,   // record N ms then play it back (mic + speaker check by ear)
  kCount
};
const char* toString(CmdType t);

struct DebugCommand {
  CmdType type = CmdType::Ping;
  uint32_t id = 0;
  RobotState state = RobotState::Idle;
  Expression expr = Expression::Idle;
  ContentKind kind = ContentKind::None;
  uint32_t ms = 0;   // 0 = command default
  uint32_t hz = 0;
  bool on = false;
  bool rtl = false;
  bool truncated = false;  // a text field was cut to fit (reported in the ack)
  char text[kMaxTextBytes + 1] = {0};    // text / say / caption / config value / turn_text
  char value[kMaxValueBytes + 1] = {0};  // show value / turn_text language
  char key[kMaxKeyBytes + 1] = {0};      // config key
};

enum class ParseError : uint8_t {
  None = 0,
  BadJson,
  NotObject,
  MissingCmd,
  UnknownCmd,
  BadParam,
  UnsupportedVersion,
  InvalidUtf8,
};
const char* toString(ParseError e);

// Parses one line. On error, *detail (if given) names the offending field.
ParseError parseCommand(const char* line, size_t len, DebugCommand& out, const char** detail);

}  // namespace robot
