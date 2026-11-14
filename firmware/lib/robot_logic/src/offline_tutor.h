// Offline learning fallback: runs on the robot alone when Wi-Fi or the AI gateway is
// unavailable. It cycles through flash-card activities on the OLED; speaking to the robot (or
// a short button tap pattern handled by the caller) advances to the next card.
//
// Arithmetic is computed here with integers — it is never generated text.
#pragma once
#include <stdint.h>

#include "robot.h"

namespace robot {

class OfflineTutor {
 public:
  enum class Activity : uint8_t { AlphabetEn = 0, AlphabetAr, Counting, Math, kCount };

  void setAge(uint8_t age) { age_ = age; }
  void seed(uint32_t s) { rng_ = s ? s : 1; }

  void start(Robot& r, uint32_t now);
  void stop() { active_ = false; }
  bool active() const { return active_; }

  void next(Robot& r, uint32_t now);
  void nextActivity(Robot& r, uint32_t now);
  void tick(Robot& r, uint32_t now);

  Activity activity() const { return act_; }
  uint16_t index() const { return index_; }

  // Exposed for tests: current sum and its verified answer.
  int a() const { return a_; }
  int b() const { return b_; }
  char op() const { return op_; }
  int answer() const;

 private:
  void newProblem();
  void show(Robot& r, uint32_t now);
  uint32_t rand32();

  bool active_ = false;
  Activity act_ = Activity::AlphabetEn;
  uint16_t index_ = 0;
  bool revealed_ = false;
  uint32_t nextAt_ = 0;
  uint32_t rng_ = 0x2545F491u;
  uint8_t age_ = 6;
  int a_ = 0, b_ = 0;
  char op_ = '+';
  char value_[24] = {0};
};

const char* toString(OfflineTutor::Activity a);

}  // namespace robot
