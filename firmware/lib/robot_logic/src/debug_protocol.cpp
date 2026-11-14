#include "debug_protocol.h"

#include <ArduinoJson.h>
#include <string.h>
#include <strings.h>

#include "utf8.h"

namespace robot {

namespace {
const char* const kCmdNames[] = {"ping",   "hello",    "status",     "state", "face",     "text",      "show",
                                 "say",    "mute",     "clear",      "selftest", "config", "config_get", "reboot",
                                 "i2c_scan", "mic_test", "tone",     "wifi_scan", "turn_text", "loopback", "read"};
static_assert(sizeof(kCmdNames) / sizeof(kCmdNames[0]) == (size_t)CmdType::kCount, "cmd names");

bool lookupCmd(const char* s, CmdType& out) {
  for (size_t i = 0; i < (size_t)CmdType::kCount; i++) {
    if (strcasecmp(s, kCmdNames[i]) == 0) {
      out = (CmdType)i;
      return true;
    }
  }
  return false;
}

// Reads an optional duration; rejects negatives/non-numbers, clamps to kMaxDurationMs.
bool readMs(JsonObjectConst o, const char* key, uint32_t& out) {
  JsonVariantConst v = o[key];
  if (v.isNull()) return true;
  if (!v.is<long>() && !v.is<unsigned long>()) return false;
  long n = v.as<long>();
  if (n < 0) return false;
  out = (uint32_t)n > kMaxDurationMs ? kMaxDurationMs : (uint32_t)n;
  return true;
}

// Copies an optional/required string field with UTF-8 validation and safe truncation.
ParseError readString(JsonObjectConst o, const char* key, char* dst, size_t cap, bool required,
                      bool allowEmpty, bool& truncated, const char** detail) {
  JsonVariantConst v = o[key];
  if (v.isNull()) {
    if (required) {
      if (detail) *detail = key;
      return ParseError::BadParam;
    }
    dst[0] = 0;
    return ParseError::None;
  }
  if (!v.is<const char*>()) {
    if (detail) *detail = key;
    return ParseError::BadParam;
  }
  const char* s = v.as<const char*>();
  if (!utf8IsValid(s)) {
    if (detail) *detail = key;
    return ParseError::InvalidUtf8;
  }
  if (!allowEmpty && s[0] == 0) {
    if (detail) *detail = key;
    return ParseError::BadParam;
  }
  if (utf8CopyTruncate(dst, cap, s)) truncated = true;
  return ParseError::None;
}

bool readBool(JsonObjectConst o, const char* key, bool& out, bool required) {
  JsonVariantConst v = o[key];
  if (v.isNull()) return !required;
  if (!v.is<bool>()) return false;
  out = v.as<bool>();
  return true;
}
}  // namespace

const char* toString(CmdType t) { return (size_t)t < (size_t)CmdType::kCount ? kCmdNames[(size_t)t] : "?"; }

const char* toString(ParseError e) {
  switch (e) {
    case ParseError::None: return "none";
    case ParseError::BadJson: return "bad_json";
    case ParseError::NotObject: return "not_object";
    case ParseError::MissingCmd: return "missing_cmd";
    case ParseError::UnknownCmd: return "unknown_cmd";
    case ParseError::BadParam: return "bad_param";
    case ParseError::UnsupportedVersion: return "unsupported_version";
    case ParseError::InvalidUtf8: return "invalid_utf8";
  }
  return "?";
}

ParseError parseCommand(const char* line, size_t len, DebugCommand& out, const char** detail) {
  out = DebugCommand();
  if (detail) *detail = nullptr;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line, len, DeserializationOption::NestingLimit(2));
  if (err) return ParseError::BadJson;
  if (!doc.is<JsonObjectConst>()) return ParseError::NotObject;
  JsonObjectConst o = doc.as<JsonObjectConst>();

  JsonVariantConst ver = o["v"];
  if (!ver.isNull() && (!ver.is<int>() || ver.as<int>() != kProtocolVersion)) {
    if (detail) *detail = "v";
    return ParseError::UnsupportedVersion;
  }

  JsonVariantConst id = o["id"];
  if (!id.isNull()) {
    if (!id.is<unsigned long>() || id.as<long>() < 0) {
      if (detail) *detail = "id";
      return ParseError::BadParam;
    }
    out.id = id.as<uint32_t>();
  }

  JsonVariantConst cmd = o["cmd"];
  if (!cmd.is<const char*>()) return ParseError::MissingCmd;
  if (!lookupCmd(cmd.as<const char*>(), out.type)) {
    if (detail) *detail = "cmd";
    return ParseError::UnknownCmd;
  }

  if (!readMs(o, "ms", out.ms)) {
    if (detail) *detail = "ms";
    return ParseError::BadParam;
  }
  if (!readBool(o, "rtl", out.rtl, false)) {
    if (detail) *detail = "rtl";
    return ParseError::BadParam;
  }

  ParseError pe = ParseError::None;
  switch (out.type) {
    case CmdType::State: {
      JsonVariantConst s = o["state"];
      if (!s.is<const char*>() || !parseState(s.as<const char*>(), out.state)) {
        if (detail) *detail = "state";
        return ParseError::BadParam;
      }
      break;
    }
    case CmdType::Face: {
      JsonVariantConst e = o["expr"];
      if (!e.is<const char*>() || !parseExpression(e.as<const char*>(), out.expr)) {
        if (detail) *detail = "expr";
        return ParseError::BadParam;
      }
      break;
    }
    case CmdType::Text:
    case CmdType::Say:
      pe = readString(o, "text", out.text, sizeof(out.text), true, false, out.truncated, detail);
      break;
    case CmdType::Read:
      pe = readString(o, "text", out.longText, sizeof(out.longText), true, false, out.truncated, detail);
      break;
    case CmdType::TurnText:
      pe = readString(o, "text", out.text, sizeof(out.text), true, false, out.truncated, detail);
      if (pe == ParseError::None) {
        pe = readString(o, "lang", out.value, sizeof(out.value), false, true, out.truncated, detail);
      }
      break;
    case CmdType::Show: {
      JsonVariantConst k = o["kind"];
      if (!k.is<const char*>() || !parseContentKind(k.as<const char*>(), out.kind)) {
        if (detail) *detail = "kind";
        return ParseError::BadParam;
      }
      pe = readString(o, "value", out.value, sizeof(out.value), true, false, out.truncated, detail);
      if (pe == ParseError::None) {
        pe = readString(o, "caption", out.text, sizeof(out.text), false, true, out.truncated, detail);
      }
      break;
    }
    case CmdType::Mute:
      if (!readBool(o, "on", out.on, true)) {
        if (detail) *detail = "on";
        return ParseError::BadParam;
      }
      break;
    case CmdType::Config: {
      bool keyTrunc = false;
      pe = readString(o, "key", out.key, sizeof(out.key), true, false, keyTrunc, detail);
      if (pe == ParseError::None && keyTrunc) {
        if (detail) *detail = "key";
        return ParseError::BadParam;
      }
      if (pe == ParseError::None) {
        // Values (passwords, tokens, URLs) must never be silently truncated.
        bool valTrunc = false;
        pe = readString(o, "value", out.text, sizeof(out.text), true, true, valTrunc, detail);
        if (pe == ParseError::None && valTrunc) {
          if (detail) *detail = "value";
          return ParseError::BadParam;
        }
      }
      break;
    }
    case CmdType::Tone: {
      JsonVariantConst hz = o["hz"];
      if (!hz.isNull()) {
        if (!hz.is<long>() || hz.as<long>() < 50 || hz.as<long>() > 8000) {
          if (detail) *detail = "hz";
          return ParseError::BadParam;
        }
        out.hz = hz.as<uint32_t>();
      }
      break;
    }
    default:
      break;
  }
  return pe;
}

}  // namespace robot
