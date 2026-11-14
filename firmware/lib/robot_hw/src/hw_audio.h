// I2S audio for the standalone robot.
//
// MicInput  : INMP441 on I2S0 (RX), 16 kHz. A capture task converts 24-bit samples to 16-bit
//             with gain + DC removal and queues 20 ms frames. Software mute STOPS the I2S
//             peripheral — muted means no audio is captured at all, not just ignored.
//
// SpeakerOutput : MAX98357A on I2S1 (TX). Reply audio is streamed into an 256 KB PSRAM stream
//             buffer by the network task and drained by a player task. The amplifier's SD pin
//             is driven LOW whenever nothing plays (no hiss, and nothing for the mic to hear).
//
// Neither device can report its own presence over I2S. MicInput therefore reports whether it
// has seen a real, varying signal; the speaker can only be confirmed by a person listening.
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/stream_buffer.h>

#include "conversation.h"

namespace robot {

class MicInput {
 public:
  static constexpr uint32_t kRate = 16000;
  static constexpr size_t kFrame = 320;  // 20 ms

  bool begin(bool rightChannel, uint8_t gain);
  void setEnabled(bool on);
  bool enabled() const { return enabled_; }
  void setGain(uint8_t g) { gain_ = g ? g : 1; }
  bool readFrame(int16_t* out, uint32_t timeoutMs);
  bool installed() const { return installed_; }
  bool signalSeen() const { return signalSeen_; }
  uint32_t droppedFrames() const { return dropped_; }
  uint32_t zeroFrames() const { return zeroFrames_; }
  uint32_t framesRead() const { return framesRead_; }

 private:
  static void taskFn(void* self);
  void run();

  QueueHandle_t queue_ = nullptr;
  volatile bool enabled_ = false;
  volatile bool running_ = false;
  bool installed_ = false;
  volatile uint8_t gain_ = 8;
  volatile bool signalSeen_ = false;
  volatile uint32_t dropped_ = 0, zeroFrames_ = 0, framesRead_ = 0;
};

class SpeakerOutput : public IAudioOutput {
 public:
  bool begin();
  bool installed() const { return installed_; }

  // Streaming API used by the gateway client (network task).
  bool beginStream(uint32_t sampleRate);
  size_t write(const uint8_t* pcm16le, size_t bytes, uint32_t timeoutMs);
  void endStream();

  void playTone(uint32_t hz, uint32_t ms);
  void setVolume(uint8_t v) { volume_ = v > 100 ? 100 : v; }

  // IAudioOutput
  bool active() const override;
  uint8_t level() const override { return level_; }
  void stop() override;

  bool stopRequested() const { return stopReq_; }
  uint32_t underruns() const { return underruns_; }

 private:
  static void taskFn(void* self);
  void run();
  void ampEnable(bool on);

  StreamBufferHandle_t sb_ = nullptr;
  StaticStreamBuffer_t sbStruct_;
  uint8_t* sbStorage_ = nullptr;
  bool installed_ = false;
  volatile bool streaming_ = false;
  volatile bool playing_ = false;
  volatile bool stopReq_ = false;
  volatile uint8_t level_ = 0;
  volatile uint8_t volume_ = 60;
  volatile uint32_t rate_ = 16000;
  volatile uint32_t pendingRate_ = 16000;
  volatile uint32_t underruns_ = 0;
  bool ampOn_ = false;
};

}  // namespace robot
