// Everything the renderer needs to draw one frame. Produced by Robot::view().
// Pointers reference Robot-owned buffers and stay valid until the next Robot call.
#pragma once
#include <stdint.h>

#include "robot_types.h"

namespace robot {

struct ViewModel {
  RobotState state = RobotState::Startup;
  Expression expression = Expression::Boot;
  uint32_t stateSince = 0;
  bool sleeping = false;

  // Indicators
  bool muted = true;
  bool micActive = false;     // capturing speech right now
  uint8_t audioLevel = 0;     // 0..100 live mic level (0 when no mic data)
  bool wifiConfigured = false;
  bool wifiConnected = false;
  bool gatewayOk = false;
  bool offline = false;       // running offline learning fallback

  // One-shot blink trigger: renderer blinks whenever this counter changes.
  uint32_t blinkSeq = 0;

  // Learning content
  ContentKind contentKind = ContentKind::None;
  const char* contentValue = "";
  const char* caption = "";
  bool captionRtl = false;

  // Full-screen text overlay (replaces the face while visible)
  const char* overlayText = "";
  bool overlayRtl = false;

  // Speaking subtitle ticker
  const char* marquee = "";
  bool marqueeRtl = false;
  uint32_t marqueeSince = 0;

  const char* errorCode = "";

  // Full answer shown as scrolling text while SPEAKING (robot without a speaker).
  const char* readingText = "";
  bool readingRtl = false;
  uint32_t readingSince = 0;       // reading view starts at this time
  uint32_t readingDurationMs = 0;  // scroll is paced to finish within this

  // Selftest progress (0 = not running)
  uint8_t selftestStep = 0;
  uint8_t selftestTotal = 0;
};

}  // namespace robot
