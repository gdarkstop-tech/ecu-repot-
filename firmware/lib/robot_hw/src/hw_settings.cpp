#include "hw_settings.h"

#include <Preferences.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace robot {

namespace {
const char* kNs = "robot";  // NVS namespace

void copy(char* dst, size_t cap, const char* src) {
  strncpy(dst, src ? src : "", cap - 1);
  dst[cap - 1] = 0;
}

bool parseInt(const char* v, long lo, long hi, long& out) {
  if (!v || !*v) return false;
  char* end = nullptr;
  long n = strtol(v, &end, 10);
  if (*end != 0 || n < lo || n > hi) return false;
  out = n;
  return true;
}

// Private (RFC 1918) IPv4 hosts may use plain http:// for local development only.
bool isPrivateHost(const char* host, size_t len) {
  char h[40];
  if (len >= sizeof(h)) return false;
  memcpy(h, host, len);
  h[len] = 0;
  int a, b, c, d;
  if (sscanf(h, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
  return a == 10 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31);
}
}  // namespace

bool SettingsStore::isSecret(const char* key) {
  return strcmp(key, "wifi_pass") == 0 || strcmp(key, "device_token") == 0;
}

bool SettingsStore::isValidGatewayUrl(const char* url) {
  if (!url || !*url) return true;  // empty = not configured
  size_t n = strlen(url);
  if (n > 128) return false;
  for (size_t i = 0; i < n; i++) {
    if (url[i] <= ' ' || url[i] > '~') return false;  // no spaces / control / non-ASCII
  }
  if (strncmp(url, "https://", 8) == 0) return n > 8;
  if (strncmp(url, "http://", 7) == 0) {
    const char* host = url + 7;
    size_t hl = strcspn(host, ":/");
    return isPrivateHost(host, hl);
  }
  return false;
}

bool SettingsStore::load(Settings& s) {
  Preferences p;
  if (!p.begin(kNs, true)) return false;  // namespace may not exist yet: defaults stay
  p.getString("wifi_ssid", s.wifiSsid, sizeof(s.wifiSsid));
  p.getString("wifi_pass", s.wifiPass, sizeof(s.wifiPass));
  p.getString("gw_url", s.gatewayUrl, sizeof(s.gatewayUrl));
  p.getString("dev_token", s.deviceToken, sizeof(s.deviceToken));
  if (p.isKey("lang")) p.getString("lang", s.lang, sizeof(s.lang));
  s.childAge = p.getUChar("age", s.childAge);
  s.volume = p.getUChar("volume", s.volume);
  s.micGain = p.getUChar("mic_gain", s.micGain);
  if (p.isKey("oled_drv")) p.getString("oled_drv", s.oledDriver, sizeof(s.oledDriver));
  if (p.isKey("mic_ch")) p.getString("mic_ch", s.micChannel, sizeof(s.micChannel));
  s.speaker = p.getBool("spk", s.speaker);
  p.end();
  return true;
}

bool SettingsStore::set(Settings& s, const char* key, const char* v, char* err, size_t errLen) {
  auto fail = [&](const char* code) {
    snprintf(err, errLen, "%s", code);
    return false;
  };
  Preferences p;
  if (!p.begin(kNs, false)) return fail("nvs_unavailable");
  bool ok = true;
  long n = 0;
  size_t len = strlen(v);
  if (strcmp(key, "wifi_ssid") == 0) {
    if (len > 32) ok = fail("ssid_too_long");
    else { copy(s.wifiSsid, sizeof(s.wifiSsid), v); p.putString("wifi_ssid", v); }
  } else if (strcmp(key, "wifi_pass") == 0) {
    if (len != 0 && (len < 8 || len > 63)) ok = fail("wifi_pass_len_8_63");
    else { copy(s.wifiPass, sizeof(s.wifiPass), v); p.putString("wifi_pass", v); }
  } else if (strcmp(key, "gateway_url") == 0) {
    if (!isValidGatewayUrl(v)) ok = fail("https_required");
    else {
      copy(s.gatewayUrl, sizeof(s.gatewayUrl), v);
      size_t l = strlen(s.gatewayUrl);
      while (l > 0 && s.gatewayUrl[l - 1] == '/') s.gatewayUrl[--l] = 0;  // no trailing slash
      p.putString("gw_url", s.gatewayUrl);
    }
  } else if (strcmp(key, "device_token") == 0) {
    bool charsOk = true;
    for (size_t i = 0; i < len; i++) {
      char c = v[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.'))
        charsOk = false;
    }
    if (len != 0 && (len < 16 || len > 128 || !charsOk)) ok = fail("bad_token_format");
    else { copy(s.deviceToken, sizeof(s.deviceToken), v); p.putString("dev_token", v); }
  } else if (strcmp(key, "lang") == 0) {
    if (strcmp(v, "auto") && strcmp(v, "en") && strcmp(v, "ar")) ok = fail("lang_auto_en_ar");
    else { copy(s.lang, sizeof(s.lang), v); p.putString("lang", v); }
  } else if (strcmp(key, "child_age") == 0) {
    if (!parseInt(v, 3, 12, n)) ok = fail("age_3_12");
    else { s.childAge = (uint8_t)n; p.putUChar("age", s.childAge); }
  } else if (strcmp(key, "volume") == 0) {
    if (!parseInt(v, 0, 100, n)) ok = fail("volume_0_100");
    else { s.volume = (uint8_t)n; p.putUChar("volume", s.volume); }
  } else if (strcmp(key, "mic_gain") == 0) {
    if (!parseInt(v, 1, 32, n)) ok = fail("mic_gain_1_32");
    else { s.micGain = (uint8_t)n; p.putUChar("mic_gain", s.micGain); }
  } else if (strcmp(key, "oled_driver") == 0) {
    if (strcmp(v, "sh1106") && strcmp(v, "ssd1306")) ok = fail("oled_sh1106_ssd1306");
    else { copy(s.oledDriver, sizeof(s.oledDriver), v); p.putString("oled_drv", v); }
  } else if (strcmp(key, "speaker") == 0) {
    if (strcmp(v, "on") && strcmp(v, "off")) ok = fail("speaker_on_off");
    else { s.speaker = strcmp(v, "on") == 0; p.putBool("spk", s.speaker); }
  } else if (strcmp(key, "mic_channel") == 0) {
    if (strcmp(v, "left") && strcmp(v, "right")) ok = fail("mic_left_right");
    else { copy(s.micChannel, sizeof(s.micChannel), v); p.putString("mic_ch", v); }
  } else {
    ok = fail("unknown_key");
  }
  p.end();
  return ok;
}

}  // namespace robot
