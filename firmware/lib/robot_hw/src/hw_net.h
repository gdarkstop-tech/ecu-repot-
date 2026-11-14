// Networking for the standalone robot.
//
// WifiManager  : non-blocking station connect with exponential backoff (2 s .. 60 s) and
//                automatic recovery when the access point drops.
// GatewayClient: implements IGateway on a dedicated FreeRTOS task (core 0):
//                  POST {gateway}/v1/turns        (audio/L16 utterance)  -> reply JSON
//                  POST {gateway}/v1/turns/text   (debug text turn)      -> reply JSON
//                  GET  {gateway}/v1/turns/{id}/audio                    -> PCM stream -> speaker
//                Security: TLS validated against pinned public root CAs; the device token is
//                sent ONLY to the configured gateway origin (audio URLs must be relative paths
//                under /v1/); every reply field is bounds-checked and allow-listed.
//                Memory: the gateway's sealed conversation token ("ctx") is kept in RAM only and
//                sent back as X-Robot-Context, so a serverless gateway (Vercel) can continue
//                the conversation. It is lost on reboot by design.
//                Bodies are read with a size cap and a stall timeout, whatever the framing
//                (Content-Length or chunked).
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "conversation.h"
#include "gateway_reply.h"
#include "hw_audio.h"
#include "hw_settings.h"

namespace robot {

class WifiManager {
 public:
  using LogFn = void (*)(const char* level, const char* msg, void* ctx);
  void begin(const Settings* s, LogFn log, void* ctx);
  void reconfigure();
  void loop(uint32_t now);
  bool connected() const { return state_ == State::Connected; }
  bool configured() const { return s_ && s_->wifiConfigured(); }
  int rssi() const;
  const char* ip() const { return ip_; }
  uint32_t reconnects() const { return reconnects_; }

  bool startScan();
  int scanResult(char* json, size_t cap);  // -1 = still running

 private:
  enum class State : uint8_t { Off, Connecting, Connected, Backoff };
  void startConnect(uint32_t now);
  const Settings* s_ = nullptr;
  LogFn log_ = nullptr;
  void* ctx_ = nullptr;
  State state_ = State::Off;
  uint32_t since_ = 0;
  uint32_t backoffMs_ = 2000;
  uint32_t reconnects_ = 0;
  char ip_[16] = "";
};

class GatewayClient : public IGateway {
 public:
  using LogFn = void (*)(const char* level, const char* msg, void* ctx);
  void begin(Settings* s, WifiManager* wifi, SpeakerOutput* spk, LogFn log, void* ctx);

  // IGateway
  bool ready() const override;
  bool startUtterance(const int16_t* pcm, size_t samples, uint32_t sampleRate) override;
  bool startText(const char* text, const char* lang) override;
  GatewayPoll poll(GatewayReply& reply, char* err, size_t errLen, bool& recoverable) override;
  void cancel() override { cancel_ = true; }

  bool lastOk() const { return lastOk_; }
  bool authRejected() const { return authRejected_; }
  void clearAuthRejected() { authRejected_ = false; }
  uint32_t lastLatencyMs() const { return lastLatencyMs_; }
  bool hasContext() const { return contextLen_ > 0; }

 private:
  enum class Job : uint8_t { None = 0, Utterance, Text };
  enum class Status : uint8_t { Idle = 0, Busy, Reply, Failed };
  static void taskFn(void* self);
  void run();
  void execute();
  bool parseReply(const char* body, size_t len, uint32_t& audioRate);
  void fail(const char* code, bool recoverable);
  void log(const char* level, const char* fmt, ...);

  Settings* s_ = nullptr;
  WifiManager* wifi_ = nullptr;
  SpeakerOutput* spk_ = nullptr;
  LogFn log_ = nullptr;
  void* ctx_ = nullptr;
  SemaphoreHandle_t mtx_ = nullptr;
  TaskHandle_t task_ = nullptr;

  volatile Job job_ = Job::None;
  volatile Status status_ = Status::Idle;
  volatile bool cancel_ = false;
  const int16_t* pcm_ = nullptr;
  size_t samples_ = 0;
  uint32_t rate_ = 16000;
  char text_[kMaxTextBytes + 1] = {0};
  char lang_[8] = {0};

  GatewayReply reply_;
  char err_[48] = {0};
  bool recoverable_ = true;
  volatile bool lastOk_ = false;
  volatile bool authRejected_ = false;
  volatile uint32_t lastLatencyMs_ = 0;

  // Used only by the gateway task.
  char* replyBuf_ = nullptr;                     // kMaxReplyBytes, in PSRAM
  char audioPath_[kMaxAudioPathLen + 1] = {0};
  char context_[kMaxContextLen + 1] = {0};       // sealed by the gateway; opaque to the robot
  volatile uint16_t contextLen_ = 0;
};

}  // namespace robot
