// Captures one utterance: a small pre-roll ring (so the first syllable is not lost while the
// VAD confirms speech) plus a linear buffer for the rest. The linear storage is supplied by the
// caller — on the ESP32 it lives in PSRAM (12 s at 16 kHz = 384 KB).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace robot {

class UtteranceBuffer {
 public:
  static constexpr size_t kMaxPreRoll = 8000;  // 0.5 s at 16 kHz

  void attach(int16_t* storage, size_t capacitySamples) {
    data_ = storage;
    cap_ = capacitySamples;
    clear();
  }
  void setPreRoll(size_t samples) { preRoll_ = samples > kMaxPreRoll ? kMaxPreRoll : samples; }

  void push(const int16_t* pcm, size_t n) {
    for (size_t i = 0; i < n; i++) {
      ring_[head_] = pcm[i];
      head_ = (head_ + 1) % kMaxPreRoll;
      if (ringCount_ < kMaxPreRoll) ringCount_++;
    }
    if (!capturing_ || !data_) return;
    size_t room = cap_ - size_;
    size_t take = n < room ? n : room;
    memcpy(data_ + size_, pcm, take * sizeof(int16_t));
    size_ += take;
    if (take < n) overflowed_ = true;
  }

  // Starts a capture whose first samples are the last `preRoll` samples already pushed.
  void start() {
    size_ = 0;
    overflowed_ = false;
    capturing_ = true;
    if (!data_) return;
    size_t pre = ringCount_ < preRoll_ ? ringCount_ : preRoll_;
    if (pre > cap_) pre = cap_;
    size_t startIdx = (head_ + kMaxPreRoll - pre) % kMaxPreRoll;
    for (size_t i = 0; i < pre; i++) data_[i] = ring_[(startIdx + i) % kMaxPreRoll];
    size_ = pre;
  }
  void stop() { capturing_ = false; }
  void clear() {
    size_ = 0;
    capturing_ = false;
    overflowed_ = false;
  }

  bool capturing() const { return capturing_; }
  const int16_t* data() const { return data_; }
  size_t size() const { return size_; }
  size_t capacity() const { return cap_; }
  bool overflowed() const { return overflowed_; }

 private:
  int16_t ring_[kMaxPreRoll] = {0};
  size_t head_ = 0;
  size_t ringCount_ = 0;
  size_t preRoll_ = 4800;  // 0.3 s
  int16_t* data_ = nullptr;
  size_t cap_ = 0;
  size_t size_ = 0;
  bool capturing_ = false;
  bool overflowed_ = false;
};

}  // namespace robot
