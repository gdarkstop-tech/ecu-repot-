// Persistent device settings in NVS (survive reflashing the app; erased only by a full
// flash erase). Secrets (Wi-Fi password, device token) are write-only over the debug link:
// they are never printed, logged, or returned by config_get.
//
// The device token is a per-robot credential for the AI gateway. It is NOT a cloud AI API
// key — those live only on the gateway server.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace robot {

struct Settings {
  char wifiSsid[33] = {0};
  char wifiPass[64] = {0};
  char gatewayUrl[129] = {0};
  char deviceToken[129] = {0};
  char lang[5] = "auto";    // auto | en | ar
  uint8_t childAge = 6;     // 3..12, drives difficulty
  uint8_t volume = 60;      // 0..100
  uint8_t micGain = 8;      // 1..32 linear gain on the INMP441 signal
  char oledDriver[8] = "sh1106";  // sh1106 | ssd1306
  char micChannel[6] = "left";    // left | right (depends on the INMP441 L/R pin)
  bool speaker = false;           // MAX98357A installed? false = answers shown as text on the OLED
  bool lightScreen = true;        // BMO look: lit screen, dark face (false: dark screen, lit face)

  bool wifiConfigured() const { return wifiSsid[0] != 0; }
  bool gatewayConfigured() const { return gatewayUrl[0] != 0 && deviceToken[0] != 0; }
};

class SettingsStore {
 public:
  bool load(Settings& s);
  // Validates and persists one key. Returns false with a short error code on rejection.
  bool set(Settings& s, const char* key, const char* value, char* err, size_t errLen);
  static bool isSecret(const char* key);
  static bool isValidGatewayUrl(const char* url);
};

}  // namespace robot
