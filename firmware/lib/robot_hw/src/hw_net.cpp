#include "hw_net.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <stdarg.h>

#include "ca_roots.h"
#include "utf8.h"

#ifndef ROBOT_FW_VERSION
#define ROBOT_FW_VERSION "0.2.0"
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

bool safeAudioPath(const char* p) {
  // Relative path on the SAME gateway only: the device token must never go anywhere else.
  if (!p || strncmp(p, "/v1/", 4) != 0 || strlen(p) > 95 || strstr(p, "..")) return false;
  for (const char* c = p; *c; c++) {
    if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '/' ||
          *c == '_' || *c == '-' || *c == '.'))
      return false;
  }
  return true;
}

bool allowedEmotion(Expression e) {
  return e == Expression::Happy || e == Expression::Sad || e == Expression::Surprised || e == Expression::Idle ||
         e == Expression::Thinking;
}
}  // namespace

void GatewayClient::begin(Settings* s, WifiManager* wifi, SpeakerOutput* spk, LogFn log, void* ctx) {
  s_ = s;
  wifi_ = wifi;
  spk_ = spk;
  log_ = log;
  ctx_ = ctx;
  mtx_ = xSemaphoreCreateMutex();
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

bool GatewayClient::parseReply(const char* body, size_t len, char* audioPath, size_t pathCap, uint32_t& audioRate) {
  JsonDocument doc;
  if (deserializeJson(doc, body, len, DeserializationOption::NestingLimit(4))) return false;
  GatewayReply r;
  r.ignore = doc["ignore"] | false;
  utf8CopyTruncate(r.lang, sizeof(r.lang), doc["lang"] | "");
  JsonObjectConst disp = doc["display"];
  const char* oled = disp["oled_text"] | "";
  if (!utf8IsValid(oled)) return false;
  utf8CopyTruncate(r.oledText, sizeof(r.oledText), oled);
  r.rtl = disp["rtl"] | false;
  JsonObjectConst content = disp["content"];
  if (!content.isNull()) {
    ContentKind k;
    const char* v = content["value"] | "";
    const char* cap = content["caption"] | "";
    if (parseContentKind(content["kind"] | "", k) && *v && utf8IsValid(v) && utf8IsValid(cap)) {
      r.contentKind = k;
      utf8CopyTruncate(r.contentValue, sizeof(r.contentValue), v);
      utf8CopyTruncate(r.contentCaption, sizeof(r.contentCaption), cap);
    }
  }
  Expression e;
  r.emotion = (parseExpression(doc["emotion"] | "happy", e) && allowedEmotion(e)) ? e : Expression::Happy;

  audioPath[0] = 0;
  JsonObjectConst audio = doc["audio"];
  if (!audio.isNull() && !r.ignore) {
    const char* fmt = audio["format"] | "";
    const char* path = audio["path"] | "";
    uint32_t rate = audio["sample_rate"] | 0;
    bool rateOk = rate == 8000 || rate == 16000 || rate == 22050 || rate == 24000;
    if (strcmp(fmt, "pcm_s16le") == 0 && rateOk && safeAudioPath(path)) {
      snprintf(audioPath, pathCap, "%s", path);
      audioRate = rate;
      r.hasAudio = true;
    } else {
      log("warn", "gateway: reply audio rejected (format/rate/path)");
    }
  }
  xSemaphoreTake(mtx_, portMAX_DELAY);
  reply_ = r;
  xSemaphoreGive(mtx_);
  return true;
}

void GatewayClient::execute() {
  uint32_t t0 = millis();
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
  String auth = String("Bearer ") + s_->deviceToken;
  http.addHeader("Authorization", auth);
  http.addHeader("X-Robot-Lang", job_ == Job::Text ? lang_ : s_->lang);
  http.addHeader("X-Robot-Age", String(s_->childAge));
  http.addHeader("X-Robot-Fw", ROBOT_FW_VERSION);

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
  int len = http.getSize();
  if (len > 8192) {
    http.end();
    fail("reply_too_large", true);
    return;
  }
  String body = http.getString();
  http.end();
  char audioPath[96];
  uint32_t audioRate = 0;
  if (!parseReply(body.c_str(), body.length(), audioPath, sizeof(audioPath), audioRate)) {
    fail("bad_reply", true);
    return;
  }
  lastLatencyMs_ = millis() - t0;
  lastOk_ = true;
  // Mark the speaker busy BEFORE publishing the reply so the conversation never sees a gap.
  if (audioPath[0]) spk_->beginStream(audioRate);
  status_ = Status::Reply;
  log("info", "gateway: reply in %lu ms", (unsigned long)lastLatencyMs_);
  if (!audioPath[0]) return;

  // ---- stream the reply audio (same origin, reusing the TLS connection when possible)
  HTTPClient ah;
  ah.setReuse(false);
  ah.setConnectTimeout(8000);
  ah.setTimeout(15000);
  if (!ah.begin(*client, String(s_->gatewayUrl) + audioPath)) {
    spk_->endStream();
    return;
  }
  ah.addHeader("Authorization", auth);
  int ac = ah.GET();
  if (ac != 200) {
    log("warn", "gateway: audio http %d", ac);
    ah.end();
    spk_->endStream();
    return;
  }
  int remaining = ah.getSize();  // gateway always sends Content-Length
  WiFiClient* stream = ah.getStreamPtr();
  static uint8_t buf[2048];
  uint32_t lastData = millis();
  while (!cancel_ && !spk_->stopRequested() && (remaining > 0 || remaining == -1)) {
    size_t avail = stream->available();
    if (avail == 0) {
      if (!ah.connected() || millis() - lastData > 10000) break;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    size_t want = avail < sizeof(buf) ? avail : sizeof(buf);
    if (remaining > 0 && want > (size_t)remaining) want = (size_t)remaining;
    int n = stream->readBytes(buf, want);
    if (n <= 0) continue;
    lastData = millis();
    size_t off = 0;
    while (off < (size_t)n && !cancel_ && !spk_->stopRequested()) {
      size_t w = spk_->write(buf + off, (size_t)n - off, 1000);  // blocks while the buffer is full
      off += w;
    }
    if (remaining > 0) remaining -= n;
  }
  ah.end();
  spk_->endStream();
}

}  // namespace robot
