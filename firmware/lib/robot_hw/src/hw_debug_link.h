// USB debug link on BOTH connectors of the DevKitC-1:
//   Serial    = UART0 via the USB-UART bridge ("COM"/"UART" port)
//   USBSerial = native USB-Serial/JTAG ("USB" port)
// Lines are framed independently per port so interleaved input cannot corrupt a command.
// Writes go to both ports, are mutex-protected (several tasks log), and never block for
// long when no host is attached.
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "line_framer.h"

namespace robot {

class DebugLink {
 public:
  using LineFn = void (*)(const char* line, size_t len, void* ctx);
  using OverflowFn = void (*)(void* ctx);

  void begin(uint32_t baud);
  void poll(LineFn onLine, OverflowFn onOverflow, void* ctx);
  void writeLine(const char* line);
  void logf(const char* level, const char* fmt, ...);

 private:
  void feed(Stream& s, LineFramer& f, LineFn onLine, OverflowFn onOverflow, void* ctx);
  LineFramer uart_;
  LineFramer usb_;
  SemaphoreHandle_t mtx_ = nullptr;
};

}  // namespace robot
