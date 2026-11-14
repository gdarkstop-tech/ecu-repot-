#include "utf8.h"

#include <string.h>

namespace robot {

static size_t seqLen(uint8_t b) {
  if (b < 0x80) return 1;
  if ((b & 0xE0) == 0xC0) return 2;
  if ((b & 0xF0) == 0xE0) return 3;
  if ((b & 0xF8) == 0xF0) return 4;
  return 0;  // continuation byte or invalid lead
}

bool utf8CopyTruncate(char* dst, size_t dstSize, const char* src) {
  if (!dst || dstSize == 0) return src && *src;
  if (!src) {
    dst[0] = 0;
    return false;
  }
  size_t srcLen = strlen(src);
  if (srcLen < dstSize) {
    memcpy(dst, src, srcLen + 1);
    return false;
  }
  size_t limit = dstSize - 1;
  // Walk back from the cut point to the start of the code point that straddles it.
  size_t cut = limit;
  while (cut > 0 && (((uint8_t)src[cut]) & 0xC0) == 0x80) cut--;
  memcpy(dst, src, cut);
  dst[cut] = 0;
  return true;
}

bool utf8IsValid(const char* s) {
  if (!s) return true;
  const uint8_t* p = (const uint8_t*)s;
  while (*p) {
    size_t n = seqLen(*p);
    if (n == 0) return false;
    for (size_t i = 1; i < n; i++) {
      if ((p[i] & 0xC0) != 0x80) return false;
    }
    p += n;
  }
  return true;
}

size_t utf8Length(const char* s) {
  size_t n = 0;
  for (const uint8_t* p = (const uint8_t*)s; p && *p; p++) {
    if ((*p & 0xC0) != 0x80) n++;
  }
  return n;
}

uint32_t readingTimeMs(const char* text, bool rtl) {
  uint32_t chars = (uint32_t)utf8Length(text);
  uint32_t ms = 2000 + chars * (rtl ? 150u : 100u);
  if (ms < 4000) ms = 4000;
  if (ms > 60000) ms = 60000;
  return ms;
}

bool utf8HasNonAscii(const char* s) {
  if (!s) return false;
  for (const uint8_t* p = (const uint8_t*)s; *p; p++) {
    if (*p >= 0x80) return true;
  }
  return false;
}

}  // namespace robot
