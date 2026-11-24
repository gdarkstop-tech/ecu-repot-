#include "hw_net.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <stdarg.h>

#include "ca_roots.h"
#include "gateway_reply.h"
#include "http_body.h"
#include "utf8.h"

#ifndef ROBOT_FW_VERSION
#define ROBOT_FW_VERSION "0.6.0"
#endif

namespace robot {

// ================================================================== Wi-Fi
void WifiManager::begin(const Settings* s, LogFn log, void* ctx) {
  s_ = s;
  log_ = log;
  ctx_ = ctx;
  WiFi.persistent(false);  // credentials live in our NVS namespace, not the Wi-Fi driver's
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);  // we own reconnection (with backoff)
  WiFi.setSleep(false);          // lower latency for audio streaming
  state_ = State::Off;
}

void WifiManager::reconfigure() {
  WiFi.disconnect();
  state_ = State::Off;
  backoffMs_ = 2000;
  ip_[0] = 0;
}

void WifiManager::startConnect(uint32_t now) {
  WiFi.disconnect();
  WiFi.begin(s_->wifiSsid, s_->wifiPass[0] ? s_->wifiPass : nullptr);
  state_ = State::Connecting;
  since_ = now;
  if (log_) log_("info", "wifi: connecting", ctx_);  // never log the SSID password
}

void WifiManager::loop(uint32_t now) {
  if (!s_) return;
  switch (state_) {
    case State::Off:
      if (configured()) startConnect(now);
      break;
    case State::Connecting:
      if (WiFi.status() == WL_CONNECTED) {
        state_ = State::Connected;
        backoffMs_ = 2000;
        snprintf(ip_, sizeof(ip_), "%s", WiFi.localIP().toString().c_str());
        if (log_) log_("info", "wifi: connected", ctx_);
      } else if (now - since_ > 20000) {
        WiFi.disconnect();
        state_ = State::Backoff;
        since_ = now;
        if (log_) log_("warn", "wifi: connect timeout, backing off", ctx_);
      }
      break;
    case State::Connected:
      if (WiFi.status() != WL_CONNECTED) {
        reconnects_++;
        ip_[0] = 0;
        state_ = State::Backoff;
        since_ = now;
        backoffMs_ = 1000;
        if (log_) log_("warn", "wifi: link lost", ctx_);
      }
      break;
    case State::Backoff:
      if (now - since_ >= backoffMs_) {
        startConnect(now);
        backoffMs_ = backoffMs_ * 2 > 60000 ? 60000 : backoffMs_ * 2;
      }
      break;
  }
}

int WifiManager::rssi() const { return connected() ? WiFi.RSSI() : 0; }

bool WifiManager::startScan() { return WiFi.scanNetworks(true, false) == WIFI_SCAN_RUNNING; }

int WifiManager::scanResult(char* out, size_t cap) {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return -1;
  JsonDocument doc;
  doc["evt"] = "wifi_scan";
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (int i = 0; i < n && i < 20; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  if (n < 0) doc["error"] = "scan_failed";
  WiFi.scanDelete();
  serializeJson(doc, out, cap);
  return n < 0 ? 0 : n;
}

// ================================================================== gateway
namespace {
volatile bool g_busy = false;
const char* kResponseHeaders[] = {"Transfer-Encoding"};
constexpr uint32_t kStallMs = 10000;  // no byte for this long: give up on the body

struct BufferSink {
  char* buf;
  size_t cap;
  size_t len;
  bool overflow;
};

size_t toBuffer(const uint8_t* p, size_t n, void* ctx) {
  BufferSink* s = static_cast<BufferSink*>(ctx);
  if (s->len + n > s->cap) {
    s->overflow = true;  // larger than any valid reply: refuse instead of allocating
    return 0;
  }
  memcpy(s->buf + s->len, p, n);
  s->len += n;
  return n;
}

struct SpeakerSink {
  SpeakerOutput* spk;
  volatile bool* cancel;
  size_t written;
};

size_t toSpeaker(const uint8_t* p, size_t n, void* ctx) {
  SpeakerSink* s = static_cast<SpeakerSink*>(ctx);
  size_t off = 0;
  while (off < n) {
    if (*s->cancel || s->spk->stopRequested()) return off;  // short write ends the stream
    size_t w = s->spk->write(p + off, n - off, 1000);       // blocks while the buffer is full
    off += w;
    s->written += w;
  }
  return n;
}

// Reads the response body through HttpBodyDecoder with a stall timeout and cancel checks.
// (HTTPClient's own reader has no stall timeout and no size cap.)
bool readBody(HTTPClient& http, HttpBodyDecoder::Sink sink, void* ctx, volatile bool& cancel) {
  HttpBodyDecoder dec;
  String te = http.header("Transfer-Encoding");
  te.toLowerCase();
  int size = http.getSize();
  if (te.indexOf("chunked") >= 0) dec.beginChunked();
  else if (size >= 0) dec.beginLength((uint32_t)size);
  else dec.beginUntilClose();
  WiFiClient* s = http.getStreamPtr();
  if (!s) return false;
  static uint8_t buf[1024];  // only the gateway task reads bodies
  uint32_t last = millis();
  while (!dec.complete()) {
    if (cancel) return false;
    int avail = s->available();
    if (avail <= 0) {
      if (!s->connected()) return dec.finishOnClose();
      if (millis() - last > kStallMs) return false;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    int n = s->read(buf, avail < (int)sizeof(buf) ? (size_t)avail : sizeof(buf));
    if (n <= 0) continue;
    last = millis();
    if (!dec.feed(buf, (size_t)n, sink, ctx)) return false;
  }
  return true;
}
}  // namespace

void GatewayClient::begin(Settings* s, WifiManager* wifi, SpeakerOutput* spk, LogFn log, void* ctx) {
  s_ = s;
  wifi_ = wifi;
  spk_ = spk;
  log_ = log;
  ctx_ = ctx;
  mtx_ = xSemaphoreCreateMutex();
  replyBuf_ = static_cast<char*>(ps_malloc(kMaxReplyBytes + 1));
  if (!replyBuf_) replyBuf_ = static_cast<char*>(malloc(kMaxReplyBytes + 1));
  // TLS handshakes need a deep stack; this task runs on core 0 away from the UI.
  xTaskCreatePinnedToCore(taskFn, "gateway", 12288, this, 4, &task_, 0);
}

void GatewayClient::log(const char* level, const char* fmt, ...) {
  if (!log_) return;
  char msg[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  log_(level, msg, ctx_);
}

bool GatewayClient::ready() const {
  return wifi_ && wifi_->connected() && s_ && s_->gatewayConfigured() && !authRejected_;
}

bool GatewayClient::startUtterance(const int16_t* pcm, size_t samples, uint32_t sampleRate) {
  if (!ready() || g_busy || !pcm || samples == 0) return false;
  pcm_ = pcm;
  samples_ = samples;
  rate_ = sampleRate;
  cancel_ = false;
  g_busy = true;
  status_ = Status::Busy;
  job_ = Job::Utterance;
  xTaskNotifyGive(task_);
  return true;
}

bool GatewayClient::startText(const char* text, const char* lang) {
  if (!ready() || g_busy) return false;
  utf8CopyTruncate(text_, sizeof(text_), text);
  snprintf(lang_, sizeof(lang_), "%s", lang && *lang ? lang : s_->lang);
  cancel_ = false;
  g_busy = true;
  status_ = Status::Busy;
  job_ = Job::Text;
  xTaskNotifyGive(task_);
  return true;
}

GatewayPoll GatewayClient::poll(GatewayReply& reply, char* err, size_t errLen, bool& recoverable) {
  switch (status_) {
    case Status::Busy:
      return GatewayPoll::Busy;
    case Status::Reply:
      xSemaphoreTake(mtx_, portMAX_DELAY);
      reply = reply_;
      xSemaphoreGive(mtx_);
      status_ = Status::Idle;
      return GatewayPoll::Reply;
    case Status::Failed:
      snprintf(err, errLen, "%s", err_);
      recoverable = recoverable_;
      status_ = Status::Idle;
      return GatewayPoll::Failed;
    default:
      return GatewayPoll::Idle;
  }
}

void GatewayClient::fail(const char* code, bool recoverable) {
  snprintf(err_, sizeof(err_), "%s", code);
  recoverable_ = recoverable;
  lastOk_ = false;
  status_ = Status::Failed;
  log("warn", "gateway: %s", code);
}

void GatewayClient::taskFn(void* self) { static_cast<GatewayClient*>(self)->run(); }

void GatewayClient::run() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (job_ != Job::None) execute();
    job_ = Job::None;
    g_busy = false;
  }
}

bool GatewayClient::parseReply(const char* body, size_t len, uint32_t& audioRate) {
  GatewayReply r;
  ContextUpdate upd = ContextUpdate::Keep;
  if (!parseGatewayReply(body, len, r, audioPath_, sizeof(audioPath_), audioRate, context_, sizeof(context_), &upd))
    return false;
  if (upd == ContextUpdate::Clear) context_[0] = 0;
  contextLen_ = (uint16_t)strlen(context_);
  if (!spk_->installed()) {
    // Never try to stream or play audio without a speaker.
    r.hasAudio = false;
    audioPath_[0] = 0;
  }
  xSemaphoreTake(mtx_, portMAX_DELAY);
  reply_ = r;
  xSemaphoreGive(mtx_);
  return true;
}

void GatewayClient::execute() {
  uint32_t t0 = millis();
  if (!replyBuf_) {
    fail("no_memory", true);
    return;
  }
  bool secure = strncmp(s_->gatewayUrl, "https://", 8) == 0;
  WiFiClientSecure tls;
  WiFiClient plain;
  WiFiClient* client = &plain;
  if (secure) {
    tls.setCACert(kGatewayRootCAs);
    tls.setHandshakeTimeout(15);
    client = &tls;
  }
  HTTPClient http;
  http.setReuse(true);
  http.setConnectTimeout(8000);
  http.setTimeout(30000);
  String url = String(s_->gatewayUrl) + (job_ == Job::Utterance ? "/v1/turns" : "/v1/turns/text");
  if (!http.begin(*client, url)) {
    fail("bad_gateway_url", false);
    return;
  }
  http.collectHeaders(kResponseHeaders, 1);
  String auth = String("Bearer ") + s_->deviceToken;
  http.addHeader("Authorization", auth);
  // The conversation so far, sealed by the gateway (lets a serverless gateway remember it).
  if (context_[0]) http.addHeader("X-Robot-Context", context_);
  http.addHeader("X-Robot-Lang", job_ == Job::Text ? lang_ : s_->lang);
  http.addHeader("X-Robot-Age", String(s_->childAge));
  http.addHeader("X-Robot-Fw", ROBOT_FW_VERSION);
  // No speaker: ask for the full answer as text and no speech audio (saves time and TTS cost).
  if (!spk_->installed()) http.addHeader("X-Robot-Output", "text");

  int code;
  if (job_ == Job::Utterance) {
    char ctype[48];
    snprintf(ctype, sizeof(ctype), "audio/L16;rate=%lu;channels=1", (unsigned long)rate_);
    http.addHeader("Content-Type", ctype);
    code = http.POST((uint8_t*)pcm_, samples_ * sizeof(int16_t));
  } else {
    JsonDocument body;
    body["text"] = text_;
    body["lang"] = lang_;
    String json;
    serializeJson(body, json);
    http.addHeader("Content-Type", "application/json");
    code = http.POST(json);
  }
  if (cancel_) {
    http.end();
    fail("cancelled", true);
    return;
  }
  if (code != 200) {
    http.end();
    if (code == 401 || code == 403) {
      authRejected_ = true;
      fail("auth_rejected", false);
    } else if (code == 413) {
      fail("audio_too_long", true);
    } else if (code == 429) {
      fail("rate_limited", true);
    } else if (code >= 500) {
      fail("gateway_unavailable", true);
    } else if (code > 0) {
      char c[24];
      snprintf(c, sizeof(c), "http_%d", code);
      fail(c, true);
    } else {
      fail(secure ? "net_or_tls_error" : "net_error", true);
    }
    return;
  }
  if (http.getSize() > (int)kMaxReplyBytes) {
    http.end();
    fail("reply_too_large", true);
    return;
  }
  BufferSink sink{replyBuf_, kMaxReplyBytes, 0, false};
  bool bodyOk = readBody(http, toBuffer, &sink, cancel_);
  http.end();
  if (cancel_) {
    fail("cancelled", true);
    return;
  }
  if (!bodyOk) {
    fail(sink.overflow ? "reply_too_large" : "reply_incomplete", true);
    return;
  }
  replyBuf_[sink.len] = 0;
  uint32_t audioRate = 0;
  if (!parseReply(replyBuf_, sink.len, audioRate)) {
    fail("bad_reply", true);
    return;
  }
  lastLatencyMs_ = millis() - t0;
  lastOk_ = true;
  // Mark the speaker busy BEFORE publishing the reply so the conversation never sees a gap.
  if (audioPath_[0]) spk_->beginStream(audioRate);
  status_ = Status::Reply;
  log("info", "gateway: reply in %lu ms (memory %s)", (unsigned long)lastLatencyMs_, context_[0] ? "on" : "off");
  if (!audioPath_[0]) return;

  // ---- stream the reply audio (same origin, reusing the TLS connection when possible)
  HTTPClient ah;
  ah.setReuse(false);
  ah.setConnectTimeout(8000);
  ah.setTimeout(15000);
  if (!ah.begin(*client, String(s_->gatewayUrl) + audioPath_)) {
    spk_->endStream(false);
    return;
  }
  ah.collectHeaders(kResponseHeaders, 1);
  ah.addHeader("Authorization", auth);
  int ac = ah.GET();
  if (ac != 200) {
    // 429 = the voice service's rate limit: the conversation shows the answer as text instead.
    log("warn", "gateway: audio http %d%s", ac, ac == 429 ? " (voice rate limit)" : "");
    ah.end();
    spk_->endStream(false);
    return;
  }
  SpeakerSink speaker{spk_, &cancel_, 0};
  if (!readBody(ah, toSpeaker, &speaker, cancel_)) log("warn", "gateway: audio stream ended early");
  ah.end();
  spk_->endStream(speaker.written > 0 || cancel_);
}

}  // namespace robot
