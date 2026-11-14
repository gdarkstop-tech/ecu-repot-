// Educational robot — minimal diagnostic firmware (PlatformIO env: diag)
//
// Purpose: prove the basics on real hardware before anything else runs.
//   1. serial output on BOTH USB ports (native USB-Serial/JTAG and the USB-UART bridge)
//   2. chip / flash / PSRAM identification, plus a real PSRAM read/write test
//   3. I2C line check and bus scan on SDA=GPIO8 / SCL=GPIO9
//   4. SH1106 init, test pattern, live blink animation
//
// Every result is one JSON object per line ("evt":"diag") so tools/verify_hardware.py
// can parse it. Nothing here claims success it did not measure.
//
// Serial commands (send a single character + Enter):  r = full report,  s = I2C rescan
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <esp_chip_info.h>
#include <esp_flash.h>

#include "board_pins.h"

static const char* kDiagVersion = "diag-1.0.0";

// ------------------------------------------------------------------ output
// Arduino-ESP32 2.0.x with ARDUINO_USB_MODE=1 and CDC_ON_BOOT=0:
//   Serial    = UART0 -> USB-UART bridge port ("COM"/"UART" connector)
//   USBSerial = HWCDC -> native USB connector
// We write to both so the board answers no matter which connector is used.
static void emit(const char* line) {
  Serial.println(line);
  USBSerial.println(line);
}

static void emitf(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  emit(buf);
}

static int readCommandChar() {
  if (Serial.available()) return Serial.read();
  if (USBSerial.available()) return USBSerial.read();
  return -1;
}

// ------------------------------------------------------------------ results
struct DiagResults {
  bool psramOk = false;
  bool flashSizeOk = false;
  bool oledFound = false;
  bool oledInitOk = false;
  uint8_t oledAddr = 0;
  uint8_t i2cFound[16];
  uint8_t i2cCount = 0;
  uint32_t frameUs = 0;
};
static DiagResults R;

static U8G2* display = nullptr;

// ------------------------------------------------------------------ chip
static void reportChip() {
  esp_chip_info_t info;
  esp_chip_info(&info);
  const char* model = info.model == CHIP_ESP32S3 ? "ESP32-S3" : "UNEXPECTED";
  uint64_t mac = ESP.getEfuseMac();
  emitf(
      "{\"evt\":\"diag\",\"stage\":\"chip\",\"fw\":\"%s\",\"model\":\"%s\",\"revision\":%d,"
      "\"cores\":%d,\"cpu_mhz\":%lu,\"wifi\":%s,\"ble\":%s,\"emb_psram_flag\":%s,"
      "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"sdk\":\"%s\",\"reset_reason\":%d}",
      kDiagVersion, model, info.revision, info.cores, (unsigned long)getCpuFrequencyMhz(),
      (info.features & CHIP_FEATURE_WIFI_BGN) ? "true" : "false",
      (info.features & CHIP_FEATURE_BLE) ? "true" : "false",
      (info.features & CHIP_FEATURE_EMB_PSRAM) ? "true" : "false", (uint8_t)(mac), (uint8_t)(mac >> 8),
      (uint8_t)(mac >> 16), (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40),
      ESP.getSdkVersion(), (int)esp_reset_reason());
}

// ------------------------------------------------------------------ flash
static void reportFlash() {
  uint32_t jedec = 0;
  esp_err_t err = esp_flash_read_id(nullptr, &jedec);
  uint32_t physical = 0;
  uint8_t capacityCode = jedec & 0xFF;
  if (err == ESP_OK && capacityCode >= 0x10 && capacityCode <= 0x20) physical = 1UL << capacityCode;
  uint32_t configured = ESP.getFlashChipSize();
  const char* mode = "UNKNOWN";
  switch (ESP.getFlashChipMode()) {
    case FM_QIO: mode = "QIO"; break;
    case FM_QOUT: mode = "QOUT"; break;
    case FM_DIO: mode = "DIO"; break;
    case FM_DOUT: mode = "DOUT"; break;
    default: break;
  }
  R.flashSizeOk = (physical == 16UL * 1024 * 1024) && (configured == physical);
  emitf(
      "{\"evt\":\"diag\",\"stage\":\"flash\",\"read_id_ok\":%s,\"jedec_id\":\"0x%06lX\","
      "\"physical_bytes\":%lu,\"configured_bytes\":%lu,\"mode\":\"%s\",\"speed_hz\":%lu,"
      "\"sketch_bytes\":%lu,\"expect_16mb\":%s}",
      err == ESP_OK ? "true" : "false", (unsigned long)jedec, (unsigned long)physical,
      (unsigned long)configured, mode, (unsigned long)ESP.getFlashChipSpeed(),
      (unsigned long)ESP.getSketchSize(), R.flashSizeOk ? "true" : "false");
}

// ------------------------------------------------------------------ psram
static void reportPsram() {
  bool found = psramFound();
  uint32_t size = ESP.getPsramSize();
  const char* test = "skipped";
  float writeMBps = 0, readMBps = 0;
  const size_t kTestBytes = 1024 * 1024;
  if (found && size >= kTestBytes) {
    uint32_t* buf = (uint32_t*)ps_malloc(kTestBytes);
    if (!buf) {
      test = "alloc_failed";
    } else {
      const size_t n = kTestBytes / 4;
      uint32_t t0 = micros();
      for (size_t i = 0; i < n; i++) buf[i] = (uint32_t)(i * 2654435761UL) ^ 0xA5A5A5A5UL;
      uint32_t t1 = micros();
      bool ok = true;
      for (size_t i = 0; i < n; i++) {
        if (buf[i] != ((uint32_t)(i * 2654435761UL) ^ 0xA5A5A5A5UL)) { ok = false; break; }
      }
      uint32_t t2 = micros();
      free(buf);
      test = ok ? "pass" : "data_mismatch";
      if (t1 > t0) writeMBps = (float)kTestBytes / (float)(t1 - t0);
      if (t2 > t1) readMBps = (float)kTestBytes / (float)(t2 - t1);
    }
  }
  R.psramOk = found && size >= 8UL * 1024 * 1024 - 64 * 1024 && strcmp(test, "pass") == 0;
  emitf(
      "{\"evt\":\"diag\",\"stage\":\"psram\",\"found\":%s,\"size_bytes\":%lu,\"free_bytes\":%lu,"
      "\"test\":\"%s\",\"test_bytes\":%u,\"write_mb_s\":%.1f,\"read_mb_s\":%.1f,"
      "\"heap_size\":%lu,\"heap_free\":%lu}",
      found ? "true" : "false", (unsigned long)size, (unsigned long)ESP.getFreePsram(), test,
      (unsigned)kTestBytes, writeMBps, readMBps, (unsigned long)ESP.getHeapSize(),
      (unsigned long)ESP.getFreeHeap());
}

// ------------------------------------------------------------------ i2c
// With the internal pull-DOWN enabled, a line reads HIGH only if something external
// pulls it up — i.e. a powered OLED module (they carry 4.7k-10k pull-ups).
static void reportI2cLines() {
  pinMode(PIN_I2C_SDA, INPUT_PULLDOWN);
  pinMode(PIN_I2C_SCL, INPUT_PULLDOWN);
  delay(5);
  int sda = digitalRead(PIN_I2C_SDA);
  int scl = digitalRead(PIN_I2C_SCL);
  pinMode(PIN_I2C_SDA, INPUT);
  pinMode(PIN_I2C_SCL, INPUT);
  emitf(
      "{\"evt\":\"diag\",\"stage\":\"i2c_lines\",\"sda_pin\":%d,\"scl_pin\":%d,"
      "\"sda_external_pullup\":%s,\"scl_external_pullup\":%s}",
      PIN_I2C_SDA, PIN_I2C_SCL, sda ? "true" : "false", scl ? "true" : "false");
}

static void scanI2c() {
  Wire.end();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);  // slow + tolerant for the scan
  Wire.setTimeOut(20);
  R.i2cCount = 0;
  char list[128] = "";
  size_t pos = 0;
  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0 && R.i2cCount < sizeof(R.i2cFound)) {
      R.i2cFound[R.i2cCount++] = addr;
      pos += snprintf(list + pos, sizeof(list) - pos, "%s\"0x%02X\"", pos ? "," : "", addr);
    }
  }
  R.oledFound = false;
  for (uint8_t i = 0; i < R.i2cCount; i++) {
    if (R.i2cFound[i] == 0x3C || R.i2cFound[i] == 0x3D) {
      R.oledFound = true;
      R.oledAddr = R.i2cFound[i];
      break;
    }
  }
  emitf("{\"evt\":\"diag\",\"stage\":\"i2c_scan\",\"hz\":100000,\"count\":%u,\"found\":[%s],"
        "\"oled_candidate\":%s}",
        R.i2cCount, list, R.oledFound ? (R.oledAddr == 0x3C ? "\"0x3C\"" : "\"0x3D\"") : "null");
}

// Reads one status byte from the controller. Reported raw — SH1106 vs SSD1306 cannot be
// told apart reliably this way, so we do not infer the driver from it.
static int readControllerStatus(uint8_t addr) {
  if (Wire.requestFrom((int)addr, 1) != 1) return -1;
  return Wire.read();
}

// ------------------------------------------------------------------ oled
static void drawEyes(int openHeight) {
  const int w = 30, cy = 30;
  int h = openHeight < 2 ? 2 : openHeight;
  int r = h > 20 ? 8 : (h > 6 ? 3 : 0);
  display->drawRBox(64 - 12 - w, cy - h / 2, w, h, r);
  display->drawRBox(64 + 12, cy - h / 2, w, h, r);
}

static void drawTestPattern(uint32_t frame, int eyeH) {
  display->clearBuffer();
  display->drawFrame(0, 0, 128, 64);  // all four edges must be visible (SH1106 column offset check)
  drawEyes(eyeH);
  display->setFont(u8g2_font_5x7_tr);
  display->drawStr(3, 61, "ROBOT DIAG");
  char buf[24];
  snprintf(buf, sizeof(buf), "0x%02X %lu", R.oledAddr, (unsigned long)frame);
  display->drawStr(125 - display->getStrWidth(buf), 61, buf);
  display->drawPixel(2 + (frame % 124), 2);  // moving dot: proves the frames keep updating
}

static void initOled() {
  if (!R.oledFound) {
    emitf("{\"evt\":\"diag\",\"stage\":\"oled\",\"ok\":false,\"error\":\"not_found_on_i2c\","
          "\"hint\":\"check VCC=3V3, GND, SDA=GPIO%d, SCL=GPIO%d, and that the module is I2C (4-pin)\"}",
          PIN_I2C_SDA, PIN_I2C_SCL);
    return;
  }
  Wire.setClock(I2C_BUS_HZ);
  int status = readControllerStatus(R.oledAddr);
  if (!display) {
    display = new U8G2_SH1106_128X64_NONAME_F_HW_I2C(U8G2_R0, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA);
  }
  display->setI2CAddress(R.oledAddr << 1);
  display->setBusClock(I2C_BUS_HZ);
  R.oledInitOk = display->begin();
  if (R.oledInitOk) {
    display->setContrast(200);
    uint32_t t0 = micros();
    drawTestPattern(0, 30);
    display->sendBuffer();
    R.frameUs = micros() - t0;
    // Confirm the controller still ACKs after the full frame was sent.
    Wire.beginTransmission(R.oledAddr);
    R.oledInitOk = (Wire.endTransmission() == 0);
  }
  emitf("{\"evt\":\"diag\",\"stage\":\"oled\",\"ok\":%s,\"addr\":\"0x%02X\",\"driver\":\"SH1106\","
        "\"bus_hz\":%d,\"frame_us\":%lu,\"status_byte\":%d}",
        R.oledInitOk ? "true" : "false", R.oledAddr, I2C_BUS_HZ, (unsigned long)R.frameUs, status);
}

// ------------------------------------------------------------------ summary
static void reportSummary() {
  char failures[160] = "";
  size_t pos = 0;
  auto add = [&](const char* f) { pos += snprintf(failures + pos, sizeof(failures) - pos, "%s\"%s\"", pos ? "," : "", f); };
  if (!R.flashSizeOk) add("flash_not_16mb");
  if (!R.psramOk) add("psram_not_8mb_or_test_failed");
  if (!R.oledFound) add("oled_not_found");
  else if (!R.oledInitOk) add("oled_init_failed");
  emitf("{\"evt\":\"diag\",\"stage\":\"summary\",\"fw\":\"%s\",\"result\":\"%s\",\"failures\":[%s],"
        "\"uptime_ms\":%lu}",
        kDiagVersion, pos == 0 ? "pass" : "fail", failures, (unsigned long)millis());
}

static void fullReport() {
  reportChip();
  reportFlash();
  reportPsram();
  emitf("{\"evt\":\"diag\",\"stage\":\"i2c_bus\",\"oled_addr\":\"0x%02X\",\"found_count\":%u}", R.oledAddr,
        R.i2cCount);
  reportSummary();
}

// ------------------------------------------------------------------ arduino
void setup() {
  Serial.begin(115200);
  USBSerial.begin(115200);
  USBSerial.setTxTimeoutMs(0);  // never block if no host has the native USB port open
  uint32_t start = millis();
  while (millis() - start < 1500) delay(10);  // give a monitor time to attach

  emitf("{\"evt\":\"diag\",\"stage\":\"boot\",\"fw\":\"%s\"}", kDiagVersion);
  reportChip();
  reportFlash();
  reportPsram();
  reportI2cLines();
  scanI2c();
  initOled();
  reportSummary();
}

void loop() {
  static uint32_t lastFrame = 0, lastSummary = 0, frame = 0, blinkAt = 0;
  uint32_t now = millis();

  int c = readCommandChar();
  if (c == 'r') fullReport();
  if (c == 's') {
    reportI2cLines();
    scanI2c();
    initOled();
  }

  if (display && R.oledInitOk && now - lastFrame >= 40) {
    lastFrame = now;
    frame++;
    // Blink every 3 s: 160 ms close/open.
    if (now - blinkAt > 3000) blinkAt = now;
    uint32_t t = now - blinkAt;
    int eyeH = 30;
    if (t < 80) eyeH = 30 - (int)(t * 28 / 80);
    else if (t < 160) eyeH = 2 + (int)((t - 80) * 28 / 80);
    drawTestPattern(frame, eyeH);
    display->sendBuffer();
  }

  if (now - lastSummary >= 5000) {  // repeat so a late-attached monitor still sees the verdict
    lastSummary = now;
    reportSummary();
  }
  delay(1);
}
