// Educational robot — host simulator.
//
// Runs the firmware's portable logic (robot_logic) and renderer (robot_ui) on a PC, drawing
// into a real U8g2 SH1106 frame buffer. Two modes:
//
//   robot_sim --gallery DIR
//       Plays scripted scenes in virtual time and writes every frame to DIR/<scene>.frames
//       (raw 1 bpp, 128x64, row-major, MSB = leftmost pixel) for render_media.py.
//
//   robot_sim --serve [--snap-dir DIR]
//       Real-time: reads debug-protocol JSON lines on stdin, writes events on stdout, exactly
//       like the USB link of the real robot. Extra sim-only stdin lines:
//         #button        press the mute button
//         #snap NAME     write the current frame to DIR/NAME.pbm
//         #quit          exit
//
// The simulator has no microphone, speaker, Wi-Fi or I2C. Hardware commands are answered
// with "not_available_in_sim" — the simulator never fakes hardware results.
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "face_canvas.h"
#include "face_renderer.h"
#include "line_framer.h"
#include "robot.h"

using namespace robot;

namespace {

uint8_t nullByteCb(u8x8_t*, uint8_t, uint8_t, void*) { return 1; }
uint8_t nullGpioCb(u8x8_t*, uint8_t, uint8_t, void*) { return 1; }

void setupDisplay(u8g2_t* g) {
  // Same controller/buffer layout as the firmware (SH1106 128x64, full buffer).
  u8g2_Setup_sh1106_i2c_128x64_noname_f(g, U8G2_R0, nullByteCb, nullGpioCb);
  u8g2_InitDisplay(g);
  u8g2_SetPowerSave(g, 0);
}

// Converts the U8g2 page buffer into row-major 1 bpp (MSB first).
void toRowMajor(u8g2_t* g, uint8_t out[64 * 16]) {
  const uint8_t* buf = u8g2_GetBufferPtr(g);
  memset(out, 0, 64 * 16);
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 128; x++)
      if ((buf[(y >> 3) * 128 + x] >> (y & 7)) & 1) out[y * 16 + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
}

struct SimPlatform : IRobotPlatform {
  bool quiet = false;
  std::vector<std::pair<std::string, std::string>> settings;
  void writeLine(const char* line) override {
    if (quiet) return;
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
  }
  void getInfo(DeviceInfo& i) override {
    i.fw = "sim-0.6.0";
    i.board = "host-simulator";
    i.oledOk = true;
    i.oledAddr = 0x3C;
    i.oledDriver = "SH1106(sim)";
  }
  bool runCommand(const DebugCommand& c, char* err, size_t n) override {
    snprintf(err, n, "not_available_in_sim:%s", toString(c.type));
    return false;
  }
};

// ------------------------------------------------------------------ gallery
constexpr int kColourScale = 3;
constexpr int kColourW = 128 * kColourScale, kColourH = 64 * kColourScale;
struct Scene {
  const char* name;
  std::vector<std::string> commands;  // debug lines sent at scene start
  uint32_t durationMs;
  bool unmute;
  uint8_t micLevel;      // simulated mic level (Listening)
  bool micActive;
  bool offline;
  uint32_t advanceBeforeMs;  // extra idle time before the scene (e.g. to reach sleep)
};

void runGallery(const char* dir) {
  mkdir(dir, 0755);
  // Arabic strings are pre-shaped, visual order (what the gateway sends to the robot).
  const std::string arLetter = "ﺏ";                         // ب
  const std::string arWord = "ﺔﻄﻗ";               // قطة (cat)
  const std::string arSentence = "ﺕﻮﺑﻭﺭ ﺎﻧﺃ !ﺎﺒﺣﺮﻣ";  // مرحبا! أنا روبوت
  const std::string arMath =
      "ﺮﺸﻋ ﻲﻨﺛﺍ ﻱﻭﺎﺴﻳ ﺔﺴﻤﺧ ﺪﺋﺍﺯ "
      "ﺔﻌﺒﺳ";  // سبعة زائد خمسة يساوي اثني عشر

  const std::string arAnswer = "\u061F\uFED5\uFEAD\uFEAF\uFEF7\uFE8D \uFEE5\uFEEE\uFEE0\uFEDF\uFE8D \uFE90\uFEA4\uFE97 \uFEDE\uFEEB .\uFEE5\uFE8D\uFEEE\uFEDF\uFEF7\uFE8D \uFEF2\uFED7\uFE8E\uFE91 \uFEE6\uFEE3 \uFEAE\uFE9C\uFEDB\uFE83 \uFED5\uFEAD\uFEAF\uFEF7\uFE8D \uFE80\uFEEE\uFEC0\uFEDF\uFE8D \uFEAE\uFEB8\uFEE8\uFEF3 \uFE80\uFE8D\uFEEE\uFEEC\uFEDF\uFE8D \uFEE5\uFEF7 \uFE80\uFE8E\uFED7\uFEAD\uFEAF \uFE80\uFE8E\uFEE4\uFEB4\uFEDF\uFE8D";
  std::vector<Scene> scenes = {
      {"boot", {}, 2400, false, 0, false, false, 0},
      {"idle", {}, 6000, true, 0, false, false, 0},
      {"blink", {"{\"cmd\":\"face\",\"expr\":\"blink\"}"}, 400, true, 0, false, false, 0},
      {"happy", {"{\"cmd\":\"face\",\"expr\":\"happy\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"listening", {"{\"cmd\":\"state\",\"state\":\"LISTENING\"}"}, 2500, true, 70, true, false, 0},
      {"listening_quiet", {"{\"cmd\":\"state\",\"state\":\"LISTENING\"}"}, 2500, true, 0, false, false, 0},
      {"thinking", {"{\"cmd\":\"state\",\"state\":\"PROCESSING\"}"}, 2500, true, 0, false, false, 0},
      {"speaking", {"{\"cmd\":\"state\",\"state\":\"SPEAKING\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"speaking_subtitle_en",
       {"{\"cmd\":\"say\",\"text\":\"Seven plus five is twelve. Great job!\",\"ms\":9000}"}, 4000, true, 0, false, false, 0},
      {"speaking_subtitle_ar",
       {std::string("{\"cmd\":\"say\",\"rtl\":true,\"ms\":9000,\"text\":\"") + arMath + "\"}"}, 4000, true, 0, false, false, 0},
      {"surprised", {"{\"cmd\":\"face\",\"expr\":\"surprised\",\"ms\":9000}"}, 2000, true, 0, false, false, 0},
      {"sad", {"{\"cmd\":\"face\",\"expr\":\"sad\",\"ms\":9000}"}, 2000, true, 0, false, false, 0},
      {"excited", {"{\"cmd\":\"face\",\"expr\":\"excited\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"curious", {"{\"cmd\":\"face\",\"expr\":\"curious\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"confused", {"{\"cmd\":\"face\",\"expr\":\"confused\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"worried", {"{\"cmd\":\"face\",\"expr\":\"worried\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"playful", {"{\"cmd\":\"face\",\"expr\":\"playful\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"proud", {"{\"cmd\":\"face\",\"expr\":\"proud\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"sleepy", {"{\"cmd\":\"face\",\"expr\":\"sleepy\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"love", {"{\"cmd\":\"face\",\"expr\":\"love\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"starstruck", {"{\"cmd\":\"face\",\"expr\":\"starstruck\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"dizzy", {"{\"cmd\":\"face\",\"expr\":\"dizzy\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"laughing", {"{\"cmd\":\"face\",\"expr\":\"laughing\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"crying", {"{\"cmd\":\"face\",\"expr\":\"crying\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"annoyed", {"{\"cmd\":\"face\",\"expr\":\"annoyed\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"shy", {"{\"cmd\":\"face\",\"expr\":\"shy\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"scared", {"{\"cmd\":\"face\",\"expr\":\"scared\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"mischievous", {"{\"cmd\":\"face\",\"expr\":\"mischievous\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"bored", {"{\"cmd\":\"face\",\"expr\":\"bored\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"cool", {"{\"cmd\":\"face\",\"expr\":\"cool\",\"ms\":9000}"}, 2500, true, 0, false, false, 0},
      {"sleeping", {}, 4000, true, 0, false, false, 200000},
      {"muted", {}, 2500, false, 0, false, false, 0},
      {"error", {}, 2500, true, 0, false, false, 0},
      {"learning_letter", {"{\"cmd\":\"show\",\"kind\":\"letter\",\"value\":\"A\",\"caption\":\"A is for Apple\"}"}, 1500, true, 0, false, false, 0},
      {"learning_number", {"{\"cmd\":\"show\",\"kind\":\"number\",\"value\":\"100\",\"caption\":\"one hundred\"}"}, 1500, true, 0, false, false, 0},
      {"learning_math", {"{\"cmd\":\"show\",\"kind\":\"math\",\"value\":\"7 + 5 = ?\"}"}, 1500, true, 0, false, false, 0},
      {"learning_word", {"{\"cmd\":\"show\",\"kind\":\"word\",\"value\":\"ELEPHANT\",\"caption\":\"animal\"}"}, 1500, true, 0, false, false, 0},
      {"learning_ar_letter",
       {std::string("{\"cmd\":\"show\",\"kind\":\"letter\",\"value\":\"") + arLetter + "\",\"caption\":\"" + arWord + "\",\"rtl\":true}"}, 1500, true, 0, false, false, 0},
      {"learning_ar_word", {std::string("{\"cmd\":\"show\",\"kind\":\"word\",\"value\":\"") + arWord + "\",\"caption\":\"cat\"}"}, 1500, true, 0, false, false, 0},
      {"text_en", {"{\"cmd\":\"text\",\"text\":\"Hello! I am your learning robot. Ask me anything.\",\"ms\":9000}"}, 1500, true, 0, false, false, 0},
      {"text_ar", {std::string("{\"cmd\":\"text\",\"rtl\":true,\"ms\":9000,\"text\":\"") + arSentence + "\"}"}, 1500, true, 0, false, false, 0},
      {"offline", {}, 2500, true, 0, false, true, 0},
      // Text-only answers (robot without a speaker): paced line-by-line scrolling.
      {"reading_en",
       {"{\"cmd\":\"read\",\"ms\":9000,\"text\":\"The sky looks blue because air scatters blue sunlight more than the other colors. Do you like blue?\"}"},
       9000, true, 0, false, false, 0},
      {"reading_ar", {std::string("{\"cmd\":\"read\",\"rtl\":true,\"ms\":9000,\"text\":\"") + arAnswer + "\"}"},
       9000, true, 0, false, false, 0},
  };

  for (const Scene& sc : scenes) {
    SimPlatform plat;
    plat.quiet = true;
    StateConfig cfg;
    Robot robot(plat, cfg);
    FaceRenderer face;
    u8g2_t g;
    setupDisplay(&g);

    uint32_t t = 0;
    robot.begin(t);
    face.reset(t, 0xC0FFEEu);
    if (strcmp(sc.name, "boot") != 0) {
      // Run through startup off-camera.
      for (; t < cfg.startupMs + 100; t += 33) {
        robot.tick(t);
        face.render(&g, robot.view(t), t);
      }
      if (sc.unmute) robot.setMuted(false, t);
      if (strcmp(sc.name, "error") == 0) robot.raiseError("gateway_timeout", false, t);
      robot.setLinkStatus(true, !sc.offline, !sc.offline);
      robot.setOffline(sc.offline);
      for (uint32_t end = t + sc.advanceBeforeMs; t < end; t += 33) {
        robot.tick(t);
        face.render(&g, robot.view(t), t);
      }
      for (const std::string& c : sc.commands) robot.onDebugLine(c.c_str(), c.size(), t);
    }
    std::string path = std::string(dir) + "/" + sc.name + ".frames";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
      perror(path.c_str());
      continue;
    }
    uint8_t frame[64 * 16];
    uint32_t start = t;
    int frames = 0;
    // Colour preview (BMO's palette, 3x) of the same face, painted by the same FacePainter.
    static const char* const kNoColour[] = {"learning", "text_", "reading", "speaking_subtitle", "boot"};
    bool colour = true;
    for (const char* prefix : kNoColour)
      if (strncmp(sc.name, prefix, strlen(prefix)) == 0) colour = false;
    FILE* cf = nullptr;
    if (colour) cf = fopen((std::string(dir) + "/" + sc.name + ".rgb").c_str(), "wb");
    static uint8_t rgb[kColourW * kColourH * 3];
    for (; t < start + sc.durationMs; t += 33) {  // ~30 fps, same as the firmware
      robot.setMicActivity(sc.micActive, sc.micLevel ? (uint8_t)(sc.micLevel * (0.6 + 0.4 * ((t / 90) % 3) / 2.0)) : 0);
      robot.tick(t);
      face.render(&g, robot.view(t), t);
      toRowMajor(&g, frame);
      fwrite(frame, 1, sizeof(frame), f);
      if (cf && frames % 2 == 0) {
        RgbSurface surface(rgb, kColourW, kColourH);
        surface.setDim(face.pose().darkScreen);
        surface.clear();
        FacePainter(surface, kColourScale).draw(face.pose());
        fwrite(rgb, 1, sizeof(rgb), cf);
      }
      frames++;
    }
    fclose(f);
    if (cf) fclose(cf);
    fprintf(stderr, "scene %-22s %3d frames\n", sc.name, frames);
  }
}

// ------------------------------------------------------------------ serve
uint32_t wallMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull);
}

void writePbm(u8g2_t* g, const std::string& path) {
  uint8_t frame[64 * 16];
  toRowMajor(g, frame);
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  fprintf(f, "P4\n128 64\n");
  fwrite(frame, 1, sizeof(frame), f);
  fclose(f);
}

int runServe(const char* snapDir) {
  SimPlatform plat;
  Robot robot(plat);
  FaceRenderer face;
  u8g2_t g;
  setupDisplay(&g);
  LineFramer framer;
  uint32_t t0 = wallMs();
  robot.begin(0);
  face.reset(0);
  uint32_t lastFrame = 0;
  for (;;) {
    uint32_t now = wallMs() - t0;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(0, &rfds);
    struct timeval tv = {0, 5000};
    if (select(1, &rfds, nullptr, nullptr, &tv) > 0) {
      char buf[256];
      ssize_t n = read(0, buf, sizeof(buf));
      if (n <= 0) return 0;  // stdin closed
      for (ssize_t i = 0; i < n; i++) {
        LineFramer::Event ev = framer.push(buf[i]);
        if (ev == LineFramer::Event::Overflow) robot.onDebugOverflow(now);
        if (ev != LineFramer::Event::Line) continue;
        const char* line = framer.line();
        if (line[0] == '#') {
          if (strcmp(line, "#quit") == 0) return 0;
          if (strcmp(line, "#button") == 0) robot.onMuteButton(now);
          if (strncmp(line, "#snap ", 6) == 0 && snapDir) {
            face.render(&g, robot.view(now), now);
            writePbm(&g, std::string(snapDir) + "/" + (line + 6) + ".pbm");
            plat.writeLine("{\"evt\":\"sim\",\"snap\":true}");
          }
          continue;
        }
        robot.onDebugLine(line, framer.length(), now);
      }
    }
    robot.tick(now);
    if (now - lastFrame >= 33) {
      lastFrame = now;
      face.render(&g, robot.view(now), now);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && strcmp(argv[1], "--gallery") == 0) {
    runGallery(argv[2]);
    return 0;
  }
  if (argc >= 2 && strcmp(argv[1], "--serve") == 0) {
    const char* snap = (argc >= 4 && strcmp(argv[2], "--snap-dir") == 0) ? argv[3] : nullptr;
    if (snap) mkdir(snap, 0755);
    return runServe(snap);
  }
  fprintf(stderr, "usage: %s --gallery DIR | --serve [--snap-dir DIR]\n", argv[0]);
  return 2;
}
