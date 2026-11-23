// Educational robot — standalone firmware (PlatformIO env: robot)
//
// Runs entirely on the ESP32-S3. The laptop is NOT needed in normal operation; the USB debug
// link is only for development, testing and first-time provisioning.
//
// Task layout
//   core 1  loopTask (this file)  UI: robot logic, conversation, OLED @ ~30 fps, button, USB debug
//   core 0  "mic"                 INMP441 capture -> 20 ms frames queue
//   core 0  "spk"                 PSRAM stream buffer -> MAX98357A
//   core 0  "gateway"             HTTPS to the AI gateway (+ reply audio streaming)
//   core 0  Wi-Fi / lwIP          (ESP-IDF)
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <math.h>
#include <esp_heap_caps.h>

#include "board_pins.h"
#include "conversation.h"
#include "face_renderer.h"
#include "hw_audio.h"
#include "hw_debug_link.h"
#include "hw_display.h"
#include "hw_net.h"
#include "hw_settings.h"
#include "offline_tutor.h"
#include "robot.h"
#include "utterance_buffer.h"
#include "vad.h"

#ifndef ROBOT_FW_VERSION
#define ROBOT_FW_VERSION "0.5.0"
#endif

using namespace robot;

namespace {

constexpr uint32_t kFrameIntervalMs = 33;          // ~30 fps
constexpr size_t kUtteranceSamples = 16000 * 12;   // 12 s max utterance (PSRAM)

DebugLink g_link;
Settings g_settings;
SettingsStore g_store;
OledDisplay g_oled;
Button g_button;
MicInput g_mic;
SpeakerOutput g_spk;
WifiManager g_wifi;
GatewayClient g_gw;
FaceRenderer g_face;
Vad g_vad;
UtteranceBuffer g_utt;
OfflineTutor g_offline;
int16_t* g_uttStorage = nullptr;

// ---- debug jobs that span several loop iterations
struct MicTestJob {
  bool active = false;
  uint32_t until = 0, ms = 0, frames = 0, zeroFrames = 0;
  double sumSq = 0;
  int32_t peak = 0, minS = 0, maxS = 0;
} g_micTest;

struct LoopbackJob {
  enum Phase : uint8_t { Off, Recording, Playing } phase = Off;
  uint32_t until = 0;
  size_t samples = 0;
} g_loopback;

bool g_wifiScanPending = false;
uint32_t g_rebootAt = 0;
uint32_t g_lastStatusEvt = 0;

void logCb(const char* level, const char* msg, void*) { g_link.logf(level, "%s", msg); }

// ------------------------------------------------------------------ platform adapter
class Platform : public IRobotPlatform {
 public:
  void writeLine(const char* line) override { g_link.writeLine(line); }

  void getInfo(DeviceInfo& i) override {
    static char ip[16];
    i.fw = ROBOT_FW_VERSION;
    i.board = "ESP32-S3-DevKitC-1 N16R8";
    i.flashBytes = ESP.getFlashChipSize();
    i.psramBytes = ESP.getPsramSize();
    i.psramFree = ESP.getFreePsram();
    i.heapFree = ESP.getFreeHeap();
    i.oledOk = g_oled.present();
    i.oledAddr = g_oled.address();
    i.oledDriver = g_oled.driver();
    i.micEnabled = g_mic.enabled();
    i.micSignal = g_mic.signalSeen();
    i.speakerEnabled = g_spk.installed();
    i.speakerConfigured = g_settings.speaker;
    i.wifiConfigured = g_settings.wifiConfigured();
    i.wifiConnected = g_wifi.connected();
    i.rssi = g_wifi.rssi();
    snprintf(ip, sizeof(ip), "%s", g_wifi.ip());
    i.ip = ip;
    i.gatewayConfigured = g_settings.gatewayConfigured();
    i.gatewayOk = g_gw.lastOk();
    i.gatewayMemory = g_gw.hasContext();
    i.tokenSet = g_settings.deviceToken[0] != 0;
    i.lang = g_settings.lang;
    i.childAge = g_settings.childAge;
    i.volume = g_settings.volume;
  }

  bool runCommand(const DebugCommand& c, char* err, size_t n) override;
};

Platform g_platform;
Robot g_robot(g_platform);
Conversation g_conv(g_robot, g_vad, g_utt, g_gw, g_spk);

void emitConfig() {
  // Secrets are reported only as "set"/"not set" — never their values.
  JsonDocument doc;
  doc["evt"] = "config";
  doc["wifi_ssid"] = g_settings.wifiSsid;
  doc["wifi_pass_set"] = g_settings.wifiPass[0] != 0;
  doc["gateway_url"] = g_settings.gatewayUrl;
  doc["device_token_set"] = g_settings.deviceToken[0] != 0;
  doc["lang"] = g_settings.lang;
  doc["child_age"] = g_settings.childAge;
  doc["volume"] = g_settings.volume;
  doc["mic_gain"] = g_settings.micGain;
  doc["oled_driver"] = g_settings.oledDriver;
  doc["mic_channel"] = g_settings.micChannel;
  doc["speaker"] = g_settings.speaker ? "on" : "off";
  doc["screen"] = g_settings.lightScreen ? "light" : "dark";
  char line[512];
  serializeJson(doc, line, sizeof(line));
  g_link.writeLine(line);
}

bool Platform::runCommand(const DebugCommand& c, char* err, size_t n) {
  uint32_t now = millis();
  switch (c.type) {
    case CmdType::Config: {
      if (!g_store.set(g_settings, c.key, c.text, err, n)) return false;
      if (strncmp(c.key, "wifi_", 5) == 0) g_wifi.reconfigure();
      if (strcmp(c.key, "device_token") == 0 || strcmp(c.key, "gateway_url") == 0) g_gw.clearAuthRejected();
      if (strcmp(c.key, "volume") == 0) g_spk.setVolume(g_settings.volume);
      if (strcmp(c.key, "mic_gain") == 0) g_mic.setGain(g_settings.micGain);
      if (strcmp(c.key, "child_age") == 0) g_offline.setAge(g_settings.childAge);
      if (strcmp(c.key, "screen") == 0) g_face.setLightScreen(g_settings.lightScreen);
      bool needsReboot = strcmp(c.key, "oled_driver") == 0 || strcmp(c.key, "mic_channel") == 0 ||
                         strcmp(c.key, "speaker") == 0;
      char line[160];
      snprintf(line, sizeof(line), "{\"evt\":\"config_saved\",\"key\":\"%s\",\"secret\":%s,\"reboot_required\":%s}",
               c.key, SettingsStore::isSecret(c.key) ? "true" : "false", needsReboot ? "true" : "false");
      g_link.writeLine(line);
      return true;
    }
    case CmdType::ConfigGet:
      emitConfig();
      return true;
    case CmdType::Reboot:
      g_rebootAt = now + 300;
      return true;
    case CmdType::I2cScan: {
      char list[160] = "";
      size_t pos = 0;
      for (uint8_t a = 0x08; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0)
          pos += snprintf(list + pos, sizeof(list) - pos, "%s\"0x%02X\"", pos ? "," : "", a);
      }
      char line[220];
      snprintf(line, sizeof(line), "{\"evt\":\"i2c_scan\",\"sda\":%d,\"scl\":%d,\"found\":[%s]}", PIN_I2C_SDA,
               PIN_I2C_SCL, list);
      g_link.writeLine(line);
      return true;
    }
    case CmdType::MicTest:
      if (!g_mic.installed()) return snprintf(err, n, "mic_driver_not_installed"), false;
      if (g_micTest.active || g_loopback.phase != LoopbackJob::Off) return snprintf(err, n, "busy"), false;
      g_micTest = MicTestJob();
      g_micTest.active = true;
      g_micTest.ms = c.ms ? (c.ms > 10000 ? 10000 : c.ms) : 3000;
      g_micTest.until = now + g_micTest.ms;
      g_micTest.minS = INT32_MAX;
      g_micTest.maxS = INT32_MIN;
      return true;
    case CmdType::Tone:
      if (!g_spk.installed()) return snprintf(err, n, "speaker_driver_not_installed"), false;
      if (g_spk.active()) return snprintf(err, n, "busy"), false;
      g_spk.playTone(c.hz ? c.hz : 440, c.ms ? (c.ms > 5000 ? 5000 : c.ms) : 800);
      return true;
    case CmdType::WifiScan:
      if (!g_wifi.startScan()) return snprintf(err, n, "scan_failed"), false;
      g_wifiScanPending = true;
      return true;
    case CmdType::TurnText:
      if (!g_gw.ready()) {
        snprintf(err, n, !g_wifi.connected() ? "wifi_not_connected"
                         : !g_settings.gatewayConfigured() ? "gateway_not_configured"
                         : g_gw.authRejected() ? "auth_rejected" : "not_ready");
        return false;
      }
      if (!g_conv.submitText(c.text, c.value, now)) return snprintf(err, n, "busy"), false;
      return true;
    case CmdType::Loopback:
      if (!g_mic.installed() || !g_spk.installed() || !g_uttStorage) return snprintf(err, n, "audio_not_ready"), false;
      if (g_micTest.active || g_loopback.phase != LoopbackJob::Off || g_spk.active())
        return snprintf(err, n, "busy"), false;
      g_loopback.phase = LoopbackJob::Recording;
      g_loopback.samples = 0;
      g_loopback.until = now + (c.ms ? (c.ms > 8000 ? 8000 : c.ms) : 3000);
      g_link.logf("info", "loopback: recording, speak now");
      return true;
    default:
      snprintf(err, n, "unsupported");
      return false;
  }
}

// ------------------------------------------------------------------ debug link callbacks
void onDebugLine(const char* line, size_t len, void*) { g_robot.onDebugLine(line, len, millis()); }
void onDebugOverflow(void*) { g_robot.onDebugOverflow(millis()); }

void onOfflineSpeech(uint32_t now, void*) {
  // Offline: we cannot understand speech, but we can respond — next card, happy face.
  if (!g_offline.active()) g_offline.start(g_robot, now);
  else g_offline.next(g_robot, now);
  g_robot.setExpressionOverride(Expression::Happy, now, 1200);
}

// ------------------------------------------------------------------ async debug jobs
void finishMicTest() {
  MicTestJob& t = g_micTest;
  t.active = false;
  double rms = t.frames ? sqrt(t.sumSq / (t.frames * (double)MicInput::kFrame)) : 0;
  double db = rms > 0 ? 20.0 * log10(rms / 32768.0) : -120.0;
  const char* verdict = "signal";
  if (t.frames == 0) verdict = "no_frames";
  else if (t.zeroFrames == t.frames) verdict = "all_zero";
  else if (t.maxS - t.minS <= 4) verdict = "stuck";
  char line[300];
  snprintf(line, sizeof(line),
           "{\"evt\":\"mic_test\",\"ms\":%lu,\"frames\":%lu,\"zero_frames\":%lu,\"rms_dbfs\":%.1f,\"peak\":%ld,"
           "\"p2p\":%ld,\"verdict\":\"%s\",\"muted\":%s,\"channel\":\"%s\",\"gain\":%u}",
           (unsigned long)t.ms, (unsigned long)t.frames, (unsigned long)t.zeroFrames, db, (long)t.peak,
           (long)(t.frames ? t.maxS - t.minS : 0), verdict, g_robot.stateMachine().muted() ? "true" : "false",
           g_settings.micChannel, g_settings.micGain);
  g_link.writeLine(line);
}

void handleMicFrame(const int16_t* f, uint32_t now) {
  if (g_micTest.active) {
    g_micTest.frames++;
    bool zero = true;
    for (size_t i = 0; i < MicInput::kFrame; i++) {
      int32_t s = f[i];
      if (s) zero = false;
      g_micTest.sumSq += (double)s * s;
      int32_t a = s < 0 ? -s : s;
      if (a > g_micTest.peak) g_micTest.peak = a;
      if (s < g_micTest.minS) g_micTest.minS = s;
      if (s > g_micTest.maxS) g_micTest.maxS = s;
    }
    if (zero) g_micTest.zeroFrames++;
    return;
  }
  if (g_loopback.phase == LoopbackJob::Recording) {
    size_t room = kUtteranceSamples - g_loopback.samples;
    size_t take = room < MicInput::kFrame ? room : MicInput::kFrame;
    memcpy(g_uttStorage + g_loopback.samples, f, take * 2);
    g_loopback.samples += take;
    return;
  }
  g_conv.onMicFrame(f, MicInput::kFrame, now);
}

void tickJobs(uint32_t now) {
  if (g_micTest.active && (int32_t)(now - g_micTest.until) >= 0) finishMicTest();
  if (g_loopback.phase == LoopbackJob::Recording && (int32_t)(now - g_loopback.until) >= 0) {
    g_loopback.phase = LoopbackJob::Playing;
    g_link.logf("info", "loopback: playing %u samples", (unsigned)g_loopback.samples);
    // Stream from PSRAM into the speaker in chunks without blocking the UI for long.
    g_spk.beginStream(16000);
  }
  if (g_loopback.phase == LoopbackJob::Playing) {
    static size_t pos = 0;
    if (pos == 0 && g_loopback.samples == 0) {
      g_spk.endStream();
      g_loopback.phase = LoopbackJob::Off;
    } else {
      size_t chunk = 1024;
      if (pos + chunk > g_loopback.samples) chunk = g_loopback.samples - pos;
      pos += g_spk.write((const uint8_t*)(g_uttStorage + pos), chunk * 2, 0) / 2;
      if (pos >= g_loopback.samples) {
        g_spk.endStream();
        g_loopback.phase = LoopbackJob::Off;
        pos = 0;
        g_link.writeLine("{\"evt\":\"loopback\",\"done\":true}");
      }
    }
  }
  if (g_wifiScanPending) {
    static char json[1500];
    if (g_wifi.scanResult(json, sizeof(json)) >= 0) {
      g_wifiScanPending = false;
      g_link.writeLine(json);
    }
  }
  if (g_rebootAt && (int32_t)(now - g_rebootAt) >= 0) ESP.restart();
}

// ------------------------------------------------------------------ offline mode
void manageOffline(uint32_t now) {
  bool offline = !g_gw.ready();
  g_robot.setOffline(offline);
  RobotState st = g_robot.stateMachine().state();
  bool muted = g_robot.stateMachine().muted();
  bool idleish = st == RobotState::Idle || st == RobotState::Learning;
  bool convIdle = g_conv.phase() == Conversation::Phase::Waiting;
  if (offline && !muted && idleish && convIdle && !g_offline.active() && g_loopback.phase == LoopbackJob::Off) {
    g_offline.start(g_robot, now);
    g_link.logf("info", "offline mode: learning cards (%s)", toString(g_offline.activity()));
  } else if (g_offline.active() && (!offline || muted)) {
    g_offline.stop();
    if (st == RobotState::Learning) g_robot.requestState(RobotState::Idle, now);
  }
  if (g_offline.active() && st == RobotState::Learning) g_offline.tick(g_robot, now);
  if (g_offline.active() && st == RobotState::Idle && convIdle) g_offline.resume(g_robot, now);  // after speech
}

}  // namespace

// ================================================================== Arduino entry points
void setup() {
  g_link.begin(115200);
  uint32_t now = millis();

  bool settingsOk = g_store.load(g_settings);
  g_oled.begin(g_settings.oledDriver);
  g_face.reset(now, esp_random());
  g_face.setLightScreen(g_settings.lightScreen);
  // With a speaker the BMO face does the talking (no long text); without one, answers scroll.
  g_conv.setTextAnswers(!g_settings.speaker);
  g_robot.begin(now);
  g_link.logf("info", "boot fw=%s settings=%s oled=%s@0x%02X psram=%u", ROBOT_FW_VERSION,
              settingsOk ? "loaded" : "defaults", g_oled.present() ? "ok" : "MISSING", g_oled.address(),
              (unsigned)ESP.getPsramSize());

  g_button.begin(PIN_MUTE_BUTTON);

  // Audio: the drivers install even when the modules are absent (I2S cannot detect them);
  // presence is judged later from real signal (mic) or by a person listening (speaker).
  g_uttStorage = (int16_t*)heap_caps_malloc(kUtteranceSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (g_uttStorage) g_utt.attach(g_uttStorage, kUtteranceSamples);
  else g_link.logf("error", "PSRAM allocation for utterance buffer failed");
  bool micOk = g_mic.begin(strcmp(g_settings.micChannel, "right") == 0, g_settings.micGain);
  // Without a speaker the I2S output is not started at all (GPIO15-18 stay untouched) and
  // answers are shown as scrolling text on the OLED.
  bool spkOk = true;
  if (g_settings.speaker) {
    spkOk = g_spk.begin();
    g_spk.setVolume(g_settings.volume);
  }
  g_link.logf(micOk && spkOk ? "info" : "error", "audio: mic=%s output=%s", micOk ? "ok" : "FAILED",
              !g_settings.speaker ? "text-only (no speaker)" : (spkOk ? "speaker" : "speaker FAILED"));

  g_offline.setAge(g_settings.childAge);
  g_offline.seed(esp_random());
  g_conv.setOfflineHandler(onOfflineSpeech, nullptr);

  g_wifi.begin(&g_settings, logCb, nullptr);
  g_gw.begin(&g_settings, &g_wifi, &g_spk, logCb, nullptr);
  if (!g_settings.wifiConfigured()) g_link.logf("warn", "no Wi-Fi configured: running offline (see README: provisioning)");
}

void loop() {
  static uint32_t lastFrame = 0;
  static int16_t frame[MicInput::kFrame];
  uint32_t now = millis();

  g_link.poll(onDebugLine, onDebugOverflow, nullptr);

  switch (g_button.poll(now)) {
    case Button::Event::Short:
      g_robot.onMuteButton(now);
      break;
    case Button::Event::Long:
      if (g_offline.active()) g_offline.nextActivity(g_robot, now);
      else g_robot.blink();
      break;
    default:
      break;
  }

  // Microphone runs only when needed: muted means the I2S peripheral is stopped.
  bool wantMic = g_conv.micWanted(now) || g_micTest.active || g_loopback.phase == LoopbackJob::Recording;
  g_mic.setEnabled(wantMic);
  while (g_mic.readFrame(frame, 0)) handleMicFrame(frame, now);

  g_conv.tick(now);
  g_robot.tick(now);
  manageOffline(now);
  tickJobs(now);
  g_wifi.loop(now);
  g_robot.setLinkStatus(g_settings.wifiConfigured(), g_wifi.connected(), g_gw.ready());

  if (now - lastFrame >= kFrameIntervalMs) {
    lastFrame = now;
    g_face.render(g_oled.u8g2(), g_robot.view(now), now);
    g_oled.flush();
  }
  g_oled.maintain(now);

  // Periodic status for an attached debug host only (no noise in standalone use).
  if (g_robot.debugHostActive(now) && now - g_lastStatusEvt > 10000) {
    g_lastStatusEvt = now;
    g_robot.publishStatus(now);
  }
  delay(1);
}
