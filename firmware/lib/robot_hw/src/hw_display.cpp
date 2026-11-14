#include "hw_display.h"

#include <Arduino.h>
#include <Wire.h>
#include <string.h>

#include "board_pins.h"

namespace robot {

namespace {
volatile uint32_t g_i2cErrors = 0;

// U8g2 byte callback on top of the Arduino Wire driver, so every transfer's result is seen.
uint8_t byteCb(u8x8_t* u8x8, uint8_t msg, uint8_t argInt, void* argPtr) {
  switch (msg) {
    case U8X8_MSG_BYTE_SEND:
      Wire.write((const uint8_t*)argPtr, argInt);
      return 1;
    case U8X8_MSG_BYTE_INIT:
    case U8X8_MSG_BYTE_SET_DC:
      return 1;
    case U8X8_MSG_BYTE_START_TRANSFER:
      Wire.beginTransmission(u8x8_GetI2CAddress(u8x8) >> 1);
      return 1;
    case U8X8_MSG_BYTE_END_TRANSFER:
      if (Wire.endTransmission() != 0) g_i2cErrors++;
      return 1;
    default:
      return 0;
  }
}

uint8_t gpioCb(u8x8_t*, uint8_t msg, uint8_t argInt, void*) {
  switch (msg) {
    case U8X8_MSG_DELAY_MILLI: delay(argInt); break;
    case U8X8_MSG_DELAY_10MICRO: delayMicroseconds(10 * argInt); break;
    case U8X8_MSG_DELAY_100NANO: delayMicroseconds(1); break;
    default: break;
  }
  return 1;
}
}  // namespace

uint32_t OledDisplay::i2cErrors() const { return g_i2cErrors; }

bool OledDisplay::probe() {
  for (uint8_t a : {(uint8_t)0x3C, (uint8_t)0x3D}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      addr_ = a;
      return true;
    }
  }
  return false;
}

bool OledDisplay::setup() {
  if (strcmp(driver_, "ssd1306") == 0) {
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&g_, U8G2_R0, byteCb, gpioCb);
  } else {
    u8g2_Setup_sh1106_i2c_128x64_noname_f(&g_, U8G2_R0, byteCb, gpioCb);
  }
  u8x8_SetI2CAddress(u8g2_GetU8x8(&g_), addr_ << 1);
  uint32_t before = g_i2cErrors;
  u8g2_InitDisplay(&g_);
  u8g2_SetPowerSave(&g_, 0);
  u8g2_SetContrast(&g_, 200);
  u8g2_ClearBuffer(&g_);
  u8g2_SendBuffer(&g_);
  shadowValid_ = false;
  setupDone_ = true;
  return g_i2cErrors == before;
}

bool OledDisplay::begin(const char* driver) {
  strncpy(driver_, driver && *driver ? driver : "sh1106", sizeof(driver_) - 1);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_BUS_HZ);
  Wire.setTimeOut(20);
  present_ = probe() && setup();
  if (!setupDone_) {
    // Keep a valid frame buffer even without a display, so rendering code can always run.
    u8g2_Setup_sh1106_i2c_128x64_noname_f(&g_, U8G2_R0, byteCb, gpioCb);
  }
  nextRetry_ = millis() + 3000;
  return present_;
}

void OledDisplay::maintain(uint32_t now) {
  if (present_ || (int32_t)(now - nextRetry_) < 0) return;
  nextRetry_ = now + 3000;
  if (probe()) present_ = setup();  // hot-plug recovery
}

void OledDisplay::flush() {
  if (!present_) return;
  uint32_t t0 = micros();
  const uint8_t* buf = u8g2_GetBufferPtr(&g_);
  uint32_t before = g_i2cErrors;
  for (uint8_t page = 0; page < 8; page++) {
    const uint8_t* row = buf + page * 128;
    if (shadowValid_ && memcmp(row, shadow_ + page * 128, 128) == 0) continue;
    // Narrow the update to the changed 8-column tiles of this page.
    int first = 0, last = 15;
    if (shadowValid_) {
      while (first < 16 && memcmp(row + first * 8, shadow_ + page * 128 + first * 8, 8) == 0) first++;
      while (last > first && memcmp(row + last * 8, shadow_ + page * 128 + last * 8, 8) == 0) last--;
    }
    u8g2_UpdateDisplayArea(&g_, (uint8_t)first, page, (uint8_t)(last - first + 1), 1);
    memcpy(shadow_ + page * 128, row, 128);
  }
  shadowValid_ = true;
  lastFlushUs_ = micros() - t0;
  if (g_i2cErrors - before > 3) {
    present_ = false;  // display unplugged or bus fault: re-probe from maintain()
    shadowValid_ = false;
  }
}

// ------------------------------------------------------------------ button
void Button::begin(int pin) {
  pin_ = pin;
  pinMode(pin_, INPUT_PULLUP);  // unconnected pin reads HIGH = not pressed: safe default
}

Button::Event Button::poll(uint32_t now) {
  if (pin_ < 0) return Event::None;
  bool raw = digitalRead(pin_) == LOW;
  if (raw != lastRaw_) {
    lastRaw_ = raw;
    changedAt_ = now;
  }
  if (now - changedAt_ < 30) return Event::None;  // 30 ms debounce
  if (raw != stable_) {
    stable_ = raw;
    if (stable_) {
      pressedAt_ = now;
      longFired_ = false;
    } else if (!longFired_ && now - pressedAt_ < 1000) {
      return Event::Short;  // fire on release so a long press is never also a short one
    }
  }
  if (stable_ && !longFired_ && now - pressedAt_ >= 1200) {
    longFired_ = true;
    return Event::Long;
  }
  return Event::None;
}

}  // namespace robot
