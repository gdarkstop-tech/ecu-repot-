// Robot coordinator: owns the state machine and everything the face shows.
//
// Inputs come from three places, all funnelled through this class:
//   * local hardware events (mute button, mic level, Wi-Fi/gateway status)
//   * the conversation controller (standalone voice loop)
//   * the USB debug console (development/testing/provisioning only)
//
// Hardware access goes through IRobotPlatform, so this class is identical on the ESP32,
// in the host simulator and in unit tests.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "debug_protocol.h"
#include "state_machine.h"
#include "view_model.h"

namespace robot {

struct DeviceInfo {
  const char* fw = "";
  const char* board = "";
  uint32_t flashBytes = 0;
  uint32_t psramBytes = 0;
  uint32_t psramFree = 0;
  uint32_t heapFree = 0;
  bool oledOk = false;
  uint8_t oledAddr = 0;
  const char* oledDriver = "";
  bool micEnabled = false;      // I2S RX driver running
  bool micSignal = false;       // real, varying samples observed (not all zero / stuck)
  bool speakerEnabled = false;  // I2S TX driver running (no feedback path: cannot prove sound)
  bool speakerConfigured = false;  // "speaker" setting: false = text-only answers on the OLED
  bool wifiConfigured = false;
  bool wifiConnected = false;
  int rssi = 0;
  const char* ip = "";
  bool gatewayConfigured = false;
  bool gatewayOk = false;
  bool gatewayMemory = false;  // holding a conversation token from the gateway
  bool tokenSet = false;
  const char* lang = "auto";
  uint8_t childAge = 0;
  uint8_t volume = 0;
};

class IRobotPlatform {
 public:
  virtual ~IRobotPlatform() = default;
  // Write one line (without '\n') to the debug link. Must not block for long.
  virtual void writeLine(const char* line) = 0;
  virtual void getInfo(DeviceInfo& info) = 0;
  // Hardware / network commands (reboot, i2c_scan, mic_test, tone, wifi_scan, turn_text,
  // loopback, config, config_get). Return true if accepted; results may arrive later as events.
  virtual bool runCommand(const DebugCommand& cmd, char* err, size_t errLen) = 0;
};

class Robot {
 public:
  explicit Robot(IRobotPlatform& platform, const StateConfig& cfg = StateConfig());

  void begin(uint32_t now);
  void tick(uint32_t now);

  // ---- USB debug link
  void onDebugLine(const char* line, size_t len, uint32_t now);
  void onDebugOverflow(uint32_t now);
  bool debugHostActive(uint32_t now) const;

  // ---- local hardware events
  void onMuteButton(uint32_t now);  // toggles mute
  void setMuted(bool muted, uint32_t now);
  void setMicActivity(bool active, uint8_t level);
  void setLinkStatus(bool wifiConfigured, bool wifiConnected, bool gatewayOk);
  void setOffline(bool offline) { offline_ = offline; }

  // ---- presentation (used by the conversation controller and debug commands)
  TransitionResult requestState(RobotState s, uint32_t now, uint32_t timeoutMs = 0);
  void raiseError(const char* code, bool recoverable, uint32_t now);
  void setExpressionOverride(Expression e, uint32_t now, uint32_t ms);
  void blink() { blinkSeq_++; }
  void showContent(ContentKind k, const char* value, const char* caption, bool rtl, uint32_t now,
                   uint32_t ms);
  void clearContent();
  void showOverlayText(const char* text, bool rtl, uint32_t now, uint32_t ms);
  void setMarquee(const char* text, bool rtl, uint32_t now);
  // Show a full answer as scrolling text from `startAt` for `durationMs` (no-speaker mode).
  void showReading(const char* text, bool rtl, uint32_t startAt, uint32_t durationMs);
  void clearAll(uint32_t now);

  // Emits {"evt":...} lines on the debug link (no-op cost if nobody listens).
  void emitRaw(const char* jsonLine) { platform_.writeLine(jsonLine); }
  void emitLog(const char* level, const char* msg);
  void publishStatus(uint32_t now) { emitStatus(now); }

  const ViewModel& view(uint32_t now);
  StateMachine& stateMachine() { return sm_; }
  const StateMachine& stateMachine() const { return sm_; }

 private:
  static void onTransition(RobotState from, RobotState to, TransitionReason why, void* ctx);
  void handleCommand(const DebugCommand& cmd, uint32_t now);
  void ack(uint32_t id, bool ok, const char* err, const char* detail, bool truncated = false,
           const char* state = nullptr);
  void emitHello();
  void emitStatus(uint32_t now);
  void startSelftest(uint32_t now);
  void tickSelftest(uint32_t now);
  Expression resolveExpression(uint32_t now) const;

  IRobotPlatform& platform_;
  StateMachine sm_;
  ViewModel vm_;

  // presentation state
  bool overrideActive_ = false;
  Expression override_ = Expression::Idle;
  uint32_t overrideUntil_ = 0;
  uint32_t blinkSeq_ = 0;

  ContentKind contentKind_ = ContentKind::None;
  char contentValue_[kMaxValueBytes + 1] = {0};
  char caption_[kMaxTextBytes + 1] = {0};
  bool captionRtl_ = false;
  bool contentTimed_ = false;
  uint32_t contentUntil_ = 0;

  char overlay_[kMaxTextBytes + 1] = {0};
  bool overlayRtl_ = false;
  uint32_t overlayUntil_ = 0;

  char marquee_[kMaxTextBytes + 1] = {0};
  char reading_[kMaxLongTextBytes + 1] = {0};
  bool readingRtl_ = false;
  uint32_t readingSince_ = 0;
  uint32_t readingMs_ = 0;
  bool marqueeRtl_ = false;
  uint32_t marqueeSince_ = 0;

  bool micActive_ = false;
  uint8_t audioLevel_ = 0;
  bool wifiConfigured_ = false;
  bool wifiConnected_ = false;
  bool gatewayOk_ = false;
  bool offline_ = false;

  uint32_t lastDebugRx_ = 0;
  bool debugSeen_ = false;
  uint32_t lastNow_ = 0;

  // selftest
  uint8_t selftestStep_ = 0;  // 0 = idle, 1..N = running
  uint32_t selftestNextAt_ = 0;
};

}  // namespace robot
