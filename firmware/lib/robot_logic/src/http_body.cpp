#include "http_body.h"

namespace robot {

void HttpBodyDecoder::beginLength(uint32_t contentLength) {
  remaining_ = contentLength;
  delivered_ = 0;
  st_ = contentLength == 0 ? St::Done : St::Length;
}

void HttpBodyDecoder::beginChunked() {
  chunk_ = 0;
  digits_ = 0;
  delivered_ = 0;
  st_ = St::Size;
}

void HttpBodyDecoder::beginUntilClose() {
  delivered_ = 0;
  st_ = St::UntilClose;
}

bool HttpBodyDecoder::emit(const uint8_t* p, size_t n, Sink sink, void* ctx) {
  if (n == 0) return true;
  if (sink(p, n, ctx) != n) {
    st_ = St::Failed;
    return false;
  }
  delivered_ += n;
  return true;
}

static int hexValue(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool HttpBodyDecoder::feed(const uint8_t* p, size_t n, Sink sink, void* ctx) {
  size_t i = 0;
  while (i < n) {
    switch (st_) {
      case St::Done:
        return true;  // anything after the body belongs to no one; ignore it
      case St::Failed:
        return false;
      case St::UntilClose:
        return emit(p + i, n - i, sink, ctx);
      case St::Length:
      case St::Data: {
        size_t take = n - i < remaining_ ? n - i : remaining_;
        if (!emit(p + i, take, sink, ctx)) return false;
        i += take;
        remaining_ -= (uint32_t)take;
        if (remaining_ == 0) st_ = st_ == St::Length ? St::Done : St::DataCR;
        break;
      }
      case St::Size: {
        uint8_t c = p[i++];
        int v = hexValue(c);
        if (v >= 0) {
          if (++digits_ > 7) {  // > 256 MB chunk: not from our gateway
            st_ = St::Failed;
            return false;
          }
          chunk_ = chunk_ * 16 + (uint32_t)v;
        } else if (c == ';' || c == ' ' || c == '\t') {
          st_ = digits_ ? St::SizeExt : St::Failed;
        } else if (c == '\r') {
          st_ = digits_ ? St::SizeLF : St::Failed;
        } else if (c == '\n' && digits_) {
          st_ = St::SizeLF;
          i--;  // handle as the end of the size line
        } else {
          st_ = St::Failed;
        }
        if (st_ == St::Failed) return false;
        break;
      }
      case St::SizeExt: {
        uint8_t c = p[i++];
        if (c == '\r') st_ = St::SizeLF;
        else if (c == '\n') { st_ = St::SizeLF; i--; }
        break;
      }
      case St::SizeLF: {
        if (p[i++] != '\n') {
          st_ = St::Failed;
          return false;
        }
        if (chunk_ == 0) {
          lineLen_ = 0;
          st_ = St::Trailer;
        } else {
          remaining_ = chunk_;
          st_ = St::Data;
        }
        chunk_ = 0;
        digits_ = 0;
        break;
      }
      case St::DataCR: {
        uint8_t c = p[i++];
        if (c == '\r') st_ = St::DataLF;
        else if (c == '\n') st_ = St::Size;
        else {
          st_ = St::Failed;
          return false;
        }
        break;
      }
      case St::DataLF:
        if (p[i++] != '\n') {
          st_ = St::Failed;
          return false;
        }
        st_ = St::Size;
        break;
      case St::Trailer: {
        uint8_t c = p[i++];
        if (c == '\n') {
          if (lineLen_ == 0) st_ = St::Done;
          lineLen_ = 0;
        } else if (c != '\r') {
          if (++lineLen_ > 1024) {
            st_ = St::Failed;
            return false;
          }
        }
        break;
      }
    }
  }
  return st_ != St::Failed;
}

bool HttpBodyDecoder::finishOnClose() {
  if (st_ == St::UntilClose) st_ = St::Done;
  return st_ == St::Done;
}

}  // namespace robot
