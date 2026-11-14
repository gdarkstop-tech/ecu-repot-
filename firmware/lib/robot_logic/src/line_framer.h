// Bounded newline framer for the USB debug link.
// Never grows: an over-long line is discarded up to the next '\n' and reported once.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace robot {

class LineFramer {
 public:
  static constexpr size_t kMaxLine = 512;
  enum class Event : uint8_t { None = 0, Line, Overflow };

  Event push(char c) {
    if (ready_) {
      len_ = 0;
      ready_ = false;
    }
    if (c == '\n') {
      if (discarding_) {
        discarding_ = false;
        return Event::None;
      }
      while (len_ > 0 && (buf_[len_ - 1] == '\r' || buf_[len_ - 1] == ' ')) len_--;
      if (len_ == 0) return Event::None;
      buf_[len_] = 0;
      ready_ = true;
      return Event::Line;
    }
    if (discarding_) return Event::None;
    if (len_ >= kMaxLine) {
      discarding_ = true;
      len_ = 0;
      overflows_++;
      return Event::Overflow;
    }
    buf_[len_++] = c;
    return Event::None;
  }

  const char* line() const { return buf_; }
  size_t length() const { return ready_ ? len_ : 0; }
  uint32_t overflowCount() const { return overflows_; }
  void reset() {
    len_ = 0;
    ready_ = false;
    discarding_ = false;
  }

 private:
  char buf_[kMaxLine + 1] = {0};
  size_t len_ = 0;
  bool ready_ = false;
  bool discarding_ = false;
  uint32_t overflows_ = 0;
};

}  // namespace robot
