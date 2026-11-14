#include "hw_debug_link.h"

#include <stdarg.h>

namespace robot {

void DebugLink::begin(uint32_t baud) {
  mtx_ = xSemaphoreCreateMutex();
  Serial.setRxBufferSize(2048);
  Serial.begin(baud);
  USBSerial.setRxBufferSize(2048);
  USBSerial.begin(baud);
  USBSerial.setTxTimeoutMs(0);  // drop output instead of stalling when no USB host reads
}

void DebugLink::feed(Stream& s, LineFramer& f, LineFn onLine, OverflowFn onOverflow, void* ctx) {
  int budget = 1024;  // bound work per loop iteration
  while (budget-- > 0 && s.available() > 0) {
    int c = s.read();
    if (c < 0) break;
    LineFramer::Event ev = f.push((char)c);
    if (ev == LineFramer::Event::Line) onLine(f.line(), f.length(), ctx);
    else if (ev == LineFramer::Event::Overflow) onOverflow(ctx);
  }
}

void DebugLink::poll(LineFn onLine, OverflowFn onOverflow, void* ctx) {
  feed(Serial, uart_, onLine, onOverflow, ctx);
  feed(USBSerial, usb_, onLine, onOverflow, ctx);
}

void DebugLink::writeLine(const char* line) {
  if (mtx_) xSemaphoreTake(mtx_, pdMS_TO_TICKS(50));
  // Only write to the UART when there is room, so a missing host never stalls the robot.
  size_t n = strlen(line);
  if ((size_t)Serial.availableForWrite() > n + 2) {
    Serial.write((const uint8_t*)line, n);
    Serial.write('\n');
  }
  USBSerial.write((const uint8_t*)line, n);
  USBSerial.write('\n');
  if (mtx_) xSemaphoreGive(mtx_);
}

void DebugLink::logf(const char* level, const char* fmt, ...) {
  char msg[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  // Escape quotes/backslashes so the line stays valid JSON.
  char esc[260];
  size_t j = 0;
  for (size_t i = 0; msg[i] && j < sizeof(esc) - 2; i++) {
    char c = msg[i];
    if (c == '"' || c == '\\') esc[j++] = '\\';
    if ((unsigned char)c < 0x20) c = ' ';
    esc[j++] = c;
  }
  esc[j] = 0;
  char line[320];
  snprintf(line, sizeof(line), "{\"evt\":\"log\",\"lvl\":\"%s\",\"msg\":\"%s\"}", level, esc);
  writeLine(line);
}

}  // namespace robot
