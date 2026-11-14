#include "vad.h"

#include <math.h>

namespace robot {

const char* toString(VadEvent e) {
  switch (e) {
    case VadEvent::None: return "none";
    case VadEvent::SpeechStart: return "speech_start";
    case VadEvent::SpeechEnd: return "speech_end";
    case VadEvent::Discard: return "discard";
  }
  return "?";
}

Vad::Vad(const VadConfig& cfg) : cfg_(cfg) { reset(); }

void Vad::reset() {
  inSpeech_ = false;
  truncated_ = false;
  levelDb_ = -100.0f;
  noiseDb_ = cfg_.initialNoiseDb;
  loudMs_ = quietMs_ = speechMs_ = 0;
  framesSeen_ = 0;
}

float Vad::frameDb(const int16_t* pcm, size_t n) {
  if (!pcm || n == 0) return -100.0f;
  double acc = 0;
  for (size_t i = 0; i < n; i++) acc += (double)pcm[i] * (double)pcm[i];
  double meanSq = acc / (double)n / (32768.0 * 32768.0);
  return (float)(10.0 * log10(meanSq + 1e-10));
}

uint8_t Vad::uiLevel() const {
  float above = levelDb_ - noiseDb_;  // 0 dB .. ~40 dB above noise
  if (above <= 0) return 0;
  int v = (int)(above * 2.5f);
  return (uint8_t)(v > 100 ? 100 : v);
}

VadEvent Vad::processFrame(const int16_t* pcm, size_t n) {
  levelDb_ = frameDb(pcm, n);
  framesSeen_++;
  const uint32_t f = cfg_.frameMs;

  // Fast initial convergence: the first 0.5 s sets the floor from real room noise.
  if (framesSeen_ <= 25 && !inSpeech_ && loudMs_ == 0) {
    noiseDb_ = framesSeen_ == 1 ? levelDb_ : noiseDb_ + (levelDb_ - noiseDb_) * 0.3f;
  }

  float startThresh = noiseDb_ + cfg_.startMarginDb;
  if (startThresh < cfg_.minSpeechDb) startThresh = cfg_.minSpeechDb;
  float stopThresh = noiseDb_ + cfg_.stopMarginDb;
  if (stopThresh < cfg_.minSpeechDb - 4.0f) stopThresh = cfg_.minSpeechDb - 4.0f;

  if (!inSpeech_) {
    if (levelDb_ > startThresh) {
      loudMs_ += f;
      if (loudMs_ >= cfg_.minSpeechMs) {
        inSpeech_ = true;
        truncated_ = false;
        speechMs_ = loudMs_;
        quietMs_ = 0;
        return VadEvent::SpeechStart;
      }
    } else {
      loudMs_ = 0;
      // Adapt only on quiet frames, and only slowly upward so speech onsets do not leak in.
      float a = levelDb_ < noiseDb_ ? cfg_.noiseAdapt * 2.0f : cfg_.noiseAdapt;
      noiseDb_ += (levelDb_ - noiseDb_) * a;
    }
    return VadEvent::None;
  }

  // In speech
  speechMs_ += f;
  if (levelDb_ < stopThresh) quietMs_ += f;
  else quietMs_ = 0;

  if (speechMs_ >= cfg_.maxUtteranceMs) {
    inSpeech_ = false;
    truncated_ = true;
    loudMs_ = 0;
    // A sound that never stops (fan, TV) should not keep triggering: raise the floor to it.
    noiseDb_ = levelDb_ - cfg_.stopMarginDb * 0.5f;
    return VadEvent::SpeechEnd;
  }
  if (quietMs_ >= cfg_.hangoverMs) {
    inSpeech_ = false;
    loudMs_ = 0;
    uint32_t voiced = speechMs_ - quietMs_;
    return voiced >= cfg_.minUtteranceMs ? VadEvent::SpeechEnd : VadEvent::Discard;
  }
  return VadEvent::None;
}

}  // namespace robot
