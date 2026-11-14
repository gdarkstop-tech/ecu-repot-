#include "hw_audio.h"

#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

#include "board_pins.h"

namespace robot {

// ================================================================== microphone
bool MicInput::begin(bool rightChannel, uint8_t gain) {
  gain_ = gain ? gain : 1;
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = kRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;  // INMP441: 24-bit data in 32-bit slots
  cfg.channel_format = rightChannel ? I2S_CHANNEL_FMT_ONLY_RIGHT : I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = kFrame;
  cfg.use_apll = false;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PIN_MIC_SCK;
  pins.ws_io_num = PIN_MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = PIN_MIC_SD;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) return false;
  i2s_stop(I2S_NUM_0);  // muted at startup: the peripheral is not even clocking
  queue_ = xQueueCreate(25, sizeof(int16_t) * kFrame);  // 0.5 s of slack for the UI loop
  if (!queue_) return false;
  installed_ = true;
  xTaskCreatePinnedToCore(taskFn, "mic", 4096, this, 5, nullptr, 0);
  return true;
}

void MicInput::setEnabled(bool on) {
  if (!installed_ || on == enabled_) return;
  enabled_ = on;
  if (on) {
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_start(I2S_NUM_0);
  } else {
    i2s_stop(I2S_NUM_0);
    xQueueReset(queue_);
  }
}

bool MicInput::readFrame(int16_t* out, uint32_t timeoutMs) {
  if (!queue_) return false;
  return xQueueReceive(queue_, out, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void MicInput::taskFn(void* self) { static_cast<MicInput*>(self)->run(); }

void MicInput::run() {
  static int32_t raw[kFrame];
  static int16_t pcm[kFrame];
  float dcX = 0, dcY = 0;
  for (;;) {
    if (!enabled_) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    size_t got = 0;
    if (i2s_read(I2S_NUM_0, raw, sizeof(raw), &got, pdMS_TO_TICKS(100)) != ESP_OK || got != sizeof(raw)) continue;
    int32_t mn = INT32_MAX, mx = INT32_MIN;
    bool allZero = true;
    for (size_t i = 0; i < kFrame; i++) {
      int32_t s24 = raw[i] >> 8;  // signed 24-bit sample
      if (s24 != 0) allZero = false;
      if (s24 < mn) mn = s24;
      if (s24 > mx) mx = s24;
      float x = (float)s24 * (float)gain_ / 256.0f;  // to 16-bit scale with gain
      // One-pole DC blocker (INMP441 has a DC offset).
      float y = x - dcX + 0.995f * dcY;
      dcX = x;
      dcY = y;
      if (y > 32767.f) y = 32767.f;
      if (y < -32768.f) y = -32768.f;
      pcm[i] = (int16_t)y;
    }
    framesRead_++;
    if (allZero) zeroFrames_++;
    if (!allZero && (mx - mn) > 64) signalSeen_ = true;  // genuinely varying data
    if (xQueueSend(queue_, pcm, 0) != pdTRUE) {
      int16_t dropBuf[kFrame];
      xQueueReceive(queue_, dropBuf, 0);  // drop oldest, keep newest
      xQueueSend(queue_, pcm, 0);
      dropped_++;
    }
  }
}

// ================================================================== speaker
static constexpr size_t kSpeakerBuf = 256 * 1024;  // ~5.4 s at 24 kHz, in PSRAM

bool SpeakerOutput::begin() {
  pinMode(PIN_AMP_SD, OUTPUT);
  ampEnable(false);
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = 16000;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;  // mono duplicated to both slots
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;  // underrun plays silence, not a buzzing repeat
  if (i2s_driver_install(I2S_NUM_1, &cfg, 0, nullptr) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PIN_AMP_BCLK;
  pins.ws_io_num = PIN_AMP_LRC;
  pins.data_out_num = PIN_AMP_DIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(I2S_NUM_1, &pins) != ESP_OK) return false;
  sbStorage_ = (uint8_t*)heap_caps_malloc(kSpeakerBuf + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!sbStorage_) return false;
  sb_ = xStreamBufferCreateStatic(kSpeakerBuf, 1, sbStorage_, &sbStruct_);
  if (!sb_) return false;
  installed_ = true;
  xTaskCreatePinnedToCore(taskFn, "spk", 4096, this, 6, nullptr, 0);
  return true;
}

void SpeakerOutput::ampEnable(bool on) {
  if (on == ampOn_) return;
  ampOn_ = on;
  digitalWrite(PIN_AMP_SD, on ? HIGH : LOW);
  if (on) delay(5);  // MAX98357A turn-on time
}

bool SpeakerOutput::beginStream(uint32_t sampleRate) {
  if (!installed_) return false;
  stopReq_ = false;
  pendingRate_ = sampleRate;
  streaming_ = true;
  return true;
}

size_t SpeakerOutput::write(const uint8_t* data, size_t bytes, uint32_t timeoutMs) {
  if (!installed_ || stopReq_) return 0;
  return xStreamBufferSend(sb_, data, bytes, pdMS_TO_TICKS(timeoutMs));
}

void SpeakerOutput::endStream() { streaming_ = false; }

bool SpeakerOutput::active() const {
  if (!installed_) return false;
  return streaming_ || playing_ || xStreamBufferBytesAvailable(sb_) > 0;
}

void SpeakerOutput::stop() {
  stopReq_ = true;
  streaming_ = false;
}

void SpeakerOutput::playTone(uint32_t hz, uint32_t ms) {
  if (!beginStream(16000)) return;
  int16_t chunk[256];
  uint32_t total = 16000 * ms / 1000, n = 0;
  float phase = 0, step = 2.0f * (float)M_PI * (float)hz / 16000.0f;
  while (n < total) {
    size_t k = 0;
    for (; k < 256 && n < total; k++, n++) {
      float env = 1.0f;  // 10 ms fade in/out avoids clicks
      if (n < 160) env = n / 160.0f;
      if (total - n < 160) env = (total - n) / 160.0f;
      chunk[k] = (int16_t)(12000.0f * env * sinf(phase));
      phase += step;
    }
    if (write((const uint8_t*)chunk, k * 2, 200) == 0) break;
  }
  endStream();
}

void SpeakerOutput::taskFn(void* self) { static_cast<SpeakerOutput*>(self)->run(); }

void SpeakerOutput::run() {
  static uint8_t bytes[1024 + 1];
  static int16_t stereo[1024];
  uint32_t lastAudio = 0;
  bool hasCarry = false;  // an odd byte left over from the previous read (keeps 16-bit alignment)
  uint8_t carry = 0;
  for (;;) {
    if (stopReq_) {
      // Discard everything queued and silence the amp immediately.
      uint8_t sink[256];
      while (xStreamBufferReceive(sb_, sink, sizeof(sink), 0) > 0) {
      }
      i2s_zero_dma_buffer(I2S_NUM_1);
      ampEnable(false);
      playing_ = false;
      level_ = 0;
      hasCarry = false;
      if (!streaming_) stopReq_ = false;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (pendingRate_ != rate_ && !playing_) {
      rate_ = pendingRate_;
      i2s_set_sample_rates(I2S_NUM_1, rate_);
    }
    size_t have = 0;
    if (hasCarry) bytes[have++] = carry;
    size_t n = have + xStreamBufferReceive(sb_, bytes + have, 1024, pdMS_TO_TICKS(20));
    if (n & 1) {
      carry = bytes[n - 1];
      hasCarry = true;
      n--;
    } else {
      hasCarry = false;
    }
    if (n == 0) {
      if (playing_ && !streaming_ && millis() - lastAudio > 150) {
        i2s_zero_dma_buffer(I2S_NUM_1);
        ampEnable(false);
        playing_ = false;
        level_ = 0;
      } else if (playing_ && streaming_) {
        underruns_++;  // network slower than playback
      }
      continue;
    }
    if (!playing_) {
      ampEnable(true);
      playing_ = true;
    }
    size_t samples = n / 2;
    int32_t peak = 0;
    float vol = volume_ / 100.0f;
    vol *= vol;  // perceptual-ish volume curve
    for (size_t i = 0; i < samples; i++) {
      int16_t m;
      memcpy(&m, bytes + 2 * i, 2);  // little-endian PCM, same as the ESP32
      int32_t s = (int32_t)(m * vol);
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      int32_t a = s < 0 ? -s : s;
      if (a > peak) peak = a;
      stereo[2 * i] = (int16_t)s;
      stereo[2 * i + 1] = (int16_t)s;
    }
    // Envelope for the OLED mouth: fast attack, slower release.
    uint8_t target = (uint8_t)(peak * 100 / 20000 > 100 ? 100 : peak * 100 / 20000);
    level_ = target > level_ ? target : (uint8_t)(level_ * 0.85f);
    size_t written = 0;
    i2s_write(I2S_NUM_1, stereo, samples * 4, &written, portMAX_DELAY);
    lastAudio = millis();
  }
}

}  // namespace robot
