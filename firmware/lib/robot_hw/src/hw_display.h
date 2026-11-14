// SH1106 (or SSD1306) 128x64 I2C OLED on SDA=GPIO8 / SCL=GPIO9.
//
// * Driver chosen at runtime from settings (many "1.3 inch" modules are SH1106; a 2-pixel
//   column shift or garbage edge means the wrong one is selected).
// * Every I2C transfer is checked. On failures the display is marked lost and re-probed every
//   few seconds, so unplugging/replugging the OLED recovers without a reboot.
// * Only 8-pixel pages that changed since the last frame are sent (static screens cost no
//   bus time; animations send just the rows that move).
#pragma once
#include <stdint.h>
#include <clib/u8g2.h>

namespace robot {

class OledDisplay {
 public:
  bool begin(const char* driver);
  void flush();
  void maintain(uint32_t now);

  u8g2_t* u8g2() { return &g_; }
  bool present() const { return present_; }
  uint8_t address() const { return addr_; }
  const char* driver() const { return driver_; }
  uint32_t i2cErrors() const;
  uint32_t lastFlushUs() const { return lastFlushUs_; }

 private:
  bool probe();
  bool setup();

  u8g2_t g_;
  uint8_t shadow_[1024];
  bool shadowValid_ = false;
  bool present_ = false;
  bool setupDone_ = false;
  uint8_t addr_ = 0;
  char driver_[8] = "sh1106";
  uint32_t nextRetry_ = 0;
  uint32_t errorsAtFlush_ = 0;
  uint32_t lastFlushUs_ = 0;
};

// Debounced active-low push button. Short press (released < 1 s) and long press (held 1.2 s)
// are reported separately.
class Button {
 public:
  enum class Event : uint8_t { None = 0, Short, Long };
  void begin(int pin);
  Event poll(uint32_t now);

 private:
  int pin_ = -1;
  bool stable_ = false;  // true = pressed
  bool lastRaw_ = false;
  uint32_t changedAt_ = 0;
  uint32_t pressedAt_ = 0;
  bool longFired_ = false;
};

}  // namespace robot
