#include "robot.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

#include "utf8.h"

namespace robot {

namespace {
constexpr uint32_t kDebugHostTimeoutMs = 6000;
constexpr uint32_t kDefaultFaceMs = 2500;
constexpr uint32_t kDefaultOverlayMs = 4000;
constexpr size_t kOutBuf = 768;

struct SelftestStep {
  const char* name;
  Expression expr;
  ContentKind kind;
  const char* value;
  const char* caption;
  uint32_t ms;
};

// The order matches docs/HARDWARE_VERIFICATION_REPORT.md so a person can tick each one off.
const SelftestStep kSelftest[] = {
    {"idle", Expression::Idle, ContentKind::None, "", "", 2500},
    {"blink", Expression::Blink, ContentKind::None, "", "", 1500},
    {"happy", Expression::Happy, ContentKind::None, "", "", 2500},
    {"listening", Expression::Listening, ContentKind::None, "", "", 2500},
    {"thinking", Expression::Thinking, ContentKind::None, "", "", 2500},
    {"speaking", Expression::Speaking, ContentKind::None, "", "", 2500},
    {"surprised", Expression::Surprised, ContentKind::None, "", "", 2500},
    {"sad", Expression::Sad, ContentKind::None, "", "", 2500},
    {"sleeping", Expression::Sleeping, ContentKind::None, "", "", 3000},
    {"muted", Expression::Muted, ContentKind::None, "", "", 2500},
    {"error", Expression::Error, ContentKind::None, "", "", 2500},
    {"letter", Expression::Idle, ContentKind::Letter, "A", "Apple", 2500},
    {"number", Expression::Idle, ContentKind::Number, "42", "forty-two", 2500},
    {"math", Expression::Idle, ContentKind::Math, "7+5=12", "", 2500},
    {"word", Expression::Idle, ContentKind::Word, "CAT", "", 2500},
};
constexpr uint8_t kSelftestCount = sizeof(kSelftest) / sizeof(kSelftest[0]);

inline bool reached(uint32_t now, uint32_t when) { return (int32_t)(now - when) >= 0; }

void emitDoc(IRobotPlatform& p, JsonDocument& doc) {
  char buf[kOutBuf];
  size_t need = measureJson(doc);
  if (need >= sizeof(buf)) {
    p.writeLine("{\"evt\":\"log\",\"lvl\":\"error\",\"msg\":\"event too large\"}");
    return;
  }
  serializeJson(doc, buf, sizeof(buf));
  p.writeLine(buf);
}
}  // namespace

Robot::Robot(IRobotPlatform& platform, const StateConfig& cfg) : platform_(platform), sm_(cfg) {
  sm_.setListener(&Robot::onTransition, this);
}

void Robot::begin(uint32_t now) {
  sm_.begin(now);
  lastNow_ = now;
  emitHello();
}

// ------------------------------------------------------------------ transitions
void Robot::onTransition(RobotState from, RobotState to, TransitionReason why, void* ctx) {
  Robot* self = static_cast<Robot*>(ctx);
  if (from == RobotState::Learning && to != RobotState::Learning) self->clearContent();
  if (from == RobotState::Speaking && to != RobotState::Speaking) {
    self->marquee_[0] = 0;
    self->reading_[0] = 0;
  }
  JsonDocument doc;
  doc["evt"] = "state";
  doc["from"] = toString(from);
  doc["to"] = toString(to);
  doc["reason"] = toString(why);
  if (to == RobotState::Error) doc["error"] = self->sm_.errorCode();
  emitDoc(self->platform_, doc);
}

// ------------------------------------------------------------------ tick
void Robot::tick(uint32_t now) {
  lastNow_ = now;
  sm_.tick(now);
  if (overrideActive_ && reached(now, overrideUntil_)) overrideActive_ = false;
  if (overlay_[0] && reached(now, overlayUntil_)) overlay_[0] = 0;
  if (contentKind_ != ContentKind::None && contentTimed_ && reached(now, contentUntil_)) {
    clearContent();
    if (sm_.state() == RobotState::Learning) sm_.request(RobotState::Idle, now);
  }
  if (selftestStep_) tickSelftest(now);
}

// ------------------------------------------------------------------ debug link
bool Robot::debugHostActive(uint32_t now) const {
  return debugSeen_ && (uint32_t)(now - lastDebugRx_) < kDebugHostTimeoutMs;
}

void Robot::onDebugOverflow(uint32_t now) {
  lastDebugRx_ = now;
  debugSeen_ = true;
  ack(0, false, "line_too_long", nullptr);
}

void Robot::onDebugLine(const char* line, size_t len, uint32_t now) {
  lastDebugRx_ = now;
  debugSeen_ = true;
  DebugCommand cmd;
  const char* detail = nullptr;
  ParseError pe = parseCommand(line, len, cmd, &detail);
  if (pe != ParseError::None) {
    ack(cmd.id, false, toString(pe), detail);
    return;
  }
  handleCommand(cmd, now);
}

void Robot::ack(uint32_t id, bool ok, const char* err, const char* detail, bool truncated, const char* state) {
  JsonDocument doc;
  doc["evt"] = "ack";
  doc["id"] = id;
  doc["ok"] = ok;
  if (err) doc["err"] = err;
  if (detail) doc["detail"] = detail;
  if (truncated) doc["truncated"] = true;
  if (state) doc["state"] = state;
  emitDoc(platform_, doc);
}

void Robot::handleCommand(const DebugCommand& cmd, uint32_t now) {
  bool userFacing = cmd.type != CmdType::Ping && cmd.type != CmdType::Status && cmd.type != CmdType::Hello &&
                    cmd.type != CmdType::ConfigGet;
  if (userFacing) sm_.noteActivity(now);

  switch (cmd.type) {
    case CmdType::Ping:
      ack(cmd.id, true, nullptr, nullptr);
      return;
    case CmdType::Hello:
      ack(cmd.id, true, nullptr, nullptr);
      emitHello();
      return;
    case CmdType::Status:
      ack(cmd.id, true, nullptr, nullptr);
      emitStatus(now);
      return;
    case CmdType::State: {
      TransitionResult r = requestState(cmd.state, now, cmd.ms);
      bool ok = r == TransitionResult::Ok || r == TransitionResult::Unchanged;
      ack(cmd.id, ok, ok ? nullptr : toString(r), nullptr, false, toString(sm_.state()));
      return;
    }
    case CmdType::Face:
      if (sm_.inStartup()) {
        ack(cmd.id, false, "starting", nullptr);
        return;
      }
      if (cmd.expr == Expression::Blink) {
        blink();
      } else {
        setExpressionOverride(cmd.expr, now, cmd.ms ? cmd.ms : kDefaultFaceMs);
      }
      ack(cmd.id, true, nullptr, nullptr);
      return;
    case CmdType::Text:
      showOverlayText(cmd.text, cmd.rtl, now, cmd.ms ? cmd.ms : kDefaultOverlayMs);
      ack(cmd.id, true, nullptr, nullptr, cmd.truncated);
      return;
    case CmdType::Show: {
      if (sm_.inStartup()) {
        ack(cmd.id, false, "starting", nullptr);
        return;
      }
      showContent(cmd.kind, cmd.value, cmd.text, cmd.rtl, now, cmd.ms);
      ack(cmd.id, true, nullptr, nullptr, cmd.truncated, toString(sm_.state()));
      return;
    }
    case CmdType::Say: {
      // Visual only: the subtitle ticker plus speaking face. Audio comes from the gateway.
      uint32_t ms = cmd.ms ? cmd.ms : 1500 + 70 * (uint32_t)strlen(cmd.text);
      if (ms > 30000) ms = 30000;
      TransitionResult r = requestState(RobotState::Speaking, now, ms);
      bool ok = r == TransitionResult::Ok || r == TransitionResult::Unchanged;
      if (ok) setMarquee(cmd.text, cmd.rtl, now);
      ack(cmd.id, ok, ok ? nullptr : toString(r), nullptr, cmd.truncated, toString(sm_.state()));
      return;
    }
    case CmdType::Read: {
      uint32_t ms = cmd.ms ? cmd.ms : readingTimeMs(cmd.longText, cmd.rtl);
      TransitionResult r = requestState(RobotState::Speaking, now, ms + 500);
      bool ok = r == TransitionResult::Ok || r == TransitionResult::Unchanged;
      if (ok) showReading(cmd.longText, cmd.rtl, now, ms);
      ack(cmd.id, ok, ok ? nullptr : toString(r), nullptr, cmd.truncated, toString(sm_.state()));
      return;
    }
    case CmdType::Mute:
      setMuted(cmd.on, now);
      ack(cmd.id, true, nullptr, nullptr, false, toString(sm_.state()));
      return;
    case CmdType::Clear:
      clearAll(now);
      ack(cmd.id, true, nullptr, nullptr, false, toString(sm_.state()));
      return;
    case CmdType::Selftest:
      if (sm_.inStartup()) {
        ack(cmd.id, false, "starting", nullptr);
        return;
      }
      ack(cmd.id, true, nullptr, nullptr);
      startSelftest(now);
      return;
    default: {
      char err[48] = {0};
      bool ok = platform_.runCommand(cmd, err, sizeof(err));
      ack(cmd.id, ok, ok ? nullptr : (err[0] ? err : "unsupported"), nullptr);
      return;
    }
  }
}

// ------------------------------------------------------------------ hello / status
void Robot::emitHello() {
  DeviceInfo info;
  platform_.getInfo(info);
  JsonDocument doc;
  doc["evt"] = "hello";
  doc["proto"] = kProtocolVersion;
  doc["fw"] = info.fw;
  doc["board"] = info.board;
  doc["flash_bytes"] = info.flashBytes;
  doc["psram_bytes"] = info.psramBytes;
  doc["oled"] = info.oledOk;
  char addr[8];
  snprintf(addr, sizeof(addr), "0x%02X", info.oledAddr);
  doc["oled_addr"] = addr;
  doc["state"] = toString(sm_.state());
  emitDoc(platform_, doc);
}

void Robot::emitStatus(uint32_t now) {
  DeviceInfo info;
  platform_.getInfo(info);
  JsonDocument doc;
  doc["evt"] = "status";
  doc["state"] = toString(sm_.state());
  doc["expr"] = toString(resolveExpression(now));
  doc["muted"] = sm_.muted();
  doc["sleeping"] = sm_.sleeping(now);
  doc["offline"] = offline_;
  doc["uptime_ms"] = now;
  doc["transitions"] = sm_.transitionCount();
  if (sm_.state() == RobotState::Error) doc["error"] = sm_.errorCode();
  doc["fw"] = info.fw;
  doc["heap_free"] = info.heapFree;
  doc["psram_free"] = info.psramFree;
  JsonObject oled = doc["oled"].to<JsonObject>();
  oled["ok"] = info.oledOk;
  oled["driver"] = info.oledDriver;
  JsonObject audio = doc["audio"].to<JsonObject>();
  audio["mic_enabled"] = info.micEnabled;
  audio["mic_signal"] = info.micSignal;
  audio["speaker_enabled"] = info.speakerEnabled;
  audio["output"] = info.speakerConfigured ? "speech" : "text";
  audio["volume"] = info.volume;
  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["configured"] = info.wifiConfigured;
  wifi["connected"] = info.wifiConnected;
  wifi["rssi"] = info.rssi;
  wifi["ip"] = info.ip;
  JsonObject gw = doc["gateway"].to<JsonObject>();
  gw["configured"] = info.gatewayConfigured;
  gw["ok"] = info.gatewayOk;
  gw["memory"] = info.gatewayMemory;
  gw["token_set"] = info.tokenSet;  // the token itself is never reported
  doc["lang"] = info.lang;
  doc["child_age"] = info.childAge;
  emitDoc(platform_, doc);
}

void Robot::emitLog(const char* level, const char* msg) {
  JsonDocument doc;
  doc["evt"] = "log";
  doc["lvl"] = level;
  doc["msg"] = msg;
  emitDoc(platform_, doc);
}

// ------------------------------------------------------------------ hardware events
void Robot::onMuteButton(uint32_t now) { setMuted(!sm_.muted(), now); }

void Robot::setMuted(bool muted, uint32_t now) {
  bool changed = muted != sm_.muted();
  sm_.setMuted(muted, now);
  if (changed) {
    JsonDocument doc;
    doc["evt"] = "mute";
    doc["on"] = muted;
    emitDoc(platform_, doc);
    if (!sm_.inStartup()) blink();
  }
}

void Robot::setMicActivity(bool active, uint8_t level) {
  micActive_ = active;
  audioLevel_ = level > 100 ? 100 : level;
}

void Robot::setLinkStatus(bool wifiConfigured, bool wifiConnected, bool gatewayOk) {
  wifiConfigured_ = wifiConfigured;
  wifiConnected_ = wifiConnected;
  gatewayOk_ = gatewayOk;
}

// ------------------------------------------------------------------ presentation
TransitionResult Robot::requestState(RobotState s, uint32_t now, uint32_t timeoutMs) {
  return sm_.request(s, now, timeoutMs);
}

void Robot::raiseError(const char* code, bool recoverable, uint32_t now) { sm_.raiseError(code, recoverable, now); }

void Robot::setExpressionOverride(Expression e, uint32_t now, uint32_t ms) {
  override_ = e;
  overrideActive_ = true;
  overrideUntil_ = now + (ms ? ms : kDefaultFaceMs);
}

void Robot::showContent(ContentKind k, const char* value, const char* caption, bool rtl, uint32_t now,
                        uint32_t ms) {
  // Request the state first: entering LEARNING from another state must not wipe new content.
  if (sm_.state() != RobotState::Learning) sm_.request(RobotState::Learning, now, UINT32_MAX);
  contentKind_ = k;
  utf8CopyTruncate(contentValue_, sizeof(contentValue_), value);
  utf8CopyTruncate(caption_, sizeof(caption_), caption);
  captionRtl_ = rtl;
  contentTimed_ = ms != 0;
  contentUntil_ = now + ms;
}

void Robot::clearContent() {
  contentKind_ = ContentKind::None;
  contentValue_[0] = 0;
  caption_[0] = 0;
  contentTimed_ = false;
}

void Robot::showOverlayText(const char* text, bool rtl, uint32_t now, uint32_t ms) {
  utf8CopyTruncate(overlay_, sizeof(overlay_), text);
  overlayRtl_ = rtl;
  overlayUntil_ = now + (ms ? ms : kDefaultOverlayMs);
}

void Robot::setMarquee(const char* text, bool rtl, uint32_t now) {
  utf8CopyTruncate(marquee_, sizeof(marquee_), text);
  marqueeRtl_ = rtl;
  marqueeSince_ = now;
}

void Robot::showReading(const char* text, bool rtl, uint32_t startAt, uint32_t durationMs) {
  utf8CopyTruncate(reading_, sizeof(reading_), text);
  readingRtl_ = rtl;
  readingSince_ = startAt;
  readingMs_ = durationMs;
}

void Robot::clearAll(uint32_t now) {
  overrideActive_ = false;
  overlay_[0] = 0;
  marquee_[0] = 0;
  reading_[0] = 0;
  clearContent();
  selftestStep_ = 0;
  if (!sm_.inStartup()) sm_.request(RobotState::Idle, now);
}

// ------------------------------------------------------------------ selftest
void Robot::startSelftest(uint32_t now) {
  clearAll(now);
  selftestStep_ = 1;
  selftestNextAt_ = now;  // first step runs on the next tick
}

void Robot::tickSelftest(uint32_t now) {
  if (!reached(now, selftestNextAt_)) return;
  if (selftestStep_ > kSelftestCount) {
    selftestStep_ = 0;
    clearAll(now);
    JsonDocument doc;
    doc["evt"] = "selftest";
    doc["done"] = true;
    emitDoc(platform_, doc);
    return;
  }
  const SelftestStep& s = kSelftest[selftestStep_ - 1];
  overrideActive_ = false;
  if (s.kind != ContentKind::None) {
    showContent(s.kind, s.value, s.caption, false, now, 0);
  } else {
    if (sm_.state() == RobotState::Learning) sm_.request(RobotState::Idle, now);
    if (s.expr == Expression::Blink) {
      blink();
    } else {
      setExpressionOverride(s.expr, now, s.ms + 200);
    }
  }
  JsonDocument doc;
  doc["evt"] = "selftest";
  doc["step"] = selftestStep_;
  doc["total"] = kSelftestCount;
  doc["name"] = s.name;
  emitDoc(platform_, doc);
  selftestNextAt_ = now + s.ms;
  selftestStep_++;
}

// ------------------------------------------------------------------ view
Expression Robot::resolveExpression(uint32_t now) const {
  RobotState st = sm_.state();
  if (st == RobotState::Startup) return Expression::Boot;
  if (st == RobotState::Error) return Expression::Error;
  if (overrideActive_ && !reached(now, overrideUntil_)) return override_;
  if (sm_.sleeping(now)) return Expression::Sleeping;
  return defaultExpression(st);
}

const ViewModel& Robot::view(uint32_t now) {
  vm_.state = sm_.state();
  vm_.stateSince = sm_.stateSince();
  vm_.expression = resolveExpression(now);
  vm_.sleeping = sm_.sleeping(now);
  vm_.muted = sm_.muted();
  vm_.micActive = micActive_;
  vm_.audioLevel = audioLevel_;
  vm_.wifiConfigured = wifiConfigured_;
  vm_.wifiConnected = wifiConnected_;
  vm_.gatewayOk = gatewayOk_;
  vm_.offline = offline_;
  vm_.blinkSeq = blinkSeq_;
  vm_.contentKind = contentKind_;
  vm_.contentValue = contentValue_;
  vm_.caption = caption_;
  vm_.captionRtl = captionRtl_;
  vm_.overlayText = overlay_;
  vm_.overlayRtl = overlayRtl_;
  vm_.marquee = marquee_;
  vm_.marqueeRtl = marqueeRtl_;
  vm_.marqueeSince = marqueeSince_;
  vm_.errorCode = sm_.errorCode();
  vm_.readingText = reading_;
  vm_.readingRtl = readingRtl_;
  vm_.readingSince = readingSince_;
  vm_.readingDurationMs = readingMs_;
  vm_.selftestStep = selftestStep_ ? (uint8_t)(selftestStep_ - 1) : 0;
  vm_.selftestTotal = selftestStep_ ? kSelftestCount : 0;
  return vm_;
}

}  // namespace robot
