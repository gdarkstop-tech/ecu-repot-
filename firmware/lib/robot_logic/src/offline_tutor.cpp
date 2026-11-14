#include "offline_tutor.h"

#include <stdio.h>

#include "offline_content.h"

namespace robot {

namespace {
constexpr uint32_t kLetterMs = 4500;
constexpr uint32_t kCountMs = 2200;
constexpr uint32_t kQuestionMs = 6000;
constexpr uint32_t kAnswerMs = 3500;
inline bool reached(uint32_t now, uint32_t when) { return (int32_t)(now - when) >= 0; }
}  // namespace

const char* toString(OfflineTutor::Activity a) {
  switch (a) {
    case OfflineTutor::Activity::AlphabetEn: return "alphabet_en";
    case OfflineTutor::Activity::AlphabetAr: return "alphabet_ar";
    case OfflineTutor::Activity::Counting: return "counting";
    case OfflineTutor::Activity::Math: return "math";
    default: return "?";
  }
}

uint32_t OfflineTutor::rand32() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return rng_;
}

int OfflineTutor::answer() const {
  switch (op_) {
    case '+': return a_ + b_;
    case '-': return a_ - b_;
    case 'x': return a_ * b_;
    default: return 0;
  }
}

void OfflineTutor::newProblem() {
  // Difficulty by age: small sums for little ones, subtraction and times tables later.
  int maxN = age_ <= 5 ? 5 : (age_ <= 7 ? 10 : 20);
  uint32_t kind = age_ <= 5 ? 0 : (age_ <= 7 ? rand32() % 2 : rand32() % 3);
  if (kind == 2) {
    op_ = 'x';
    a_ = 2 + (int)(rand32() % 9);  // 2..10
    b_ = 1 + (int)(rand32() % 10);  // 1..10
    return;
  }
  a_ = (int)(rand32() % (uint32_t)(maxN + 1));
  b_ = (int)(rand32() % (uint32_t)(maxN + 1));
  if (kind == 1) {
    op_ = '-';
    if (b_ > a_) {  // keep answers non-negative for children
      int t = a_;
      a_ = b_;
      b_ = t;
    }
  } else {
    op_ = '+';
  }
}

void OfflineTutor::start(Robot& r, uint32_t now) {
  active_ = true;
  index_ = 0;
  revealed_ = false;
  if (act_ == Activity::Math) newProblem();
  show(r, now);
}

void OfflineTutor::next(Robot& r, uint32_t now) {
  if (!active_) return;
  if (act_ == Activity::Math && !revealed_) {
    revealed_ = true;  // first tap reveals the answer
  } else {
    revealed_ = false;
    index_++;
    if (act_ == Activity::Math) newProblem();
  }
  show(r, now);
}

void OfflineTutor::nextActivity(Robot& r, uint32_t now) {
  act_ = (Activity)(((uint8_t)act_ + 1) % (uint8_t)Activity::kCount);
  index_ = 0;
  revealed_ = false;
  if (act_ == Activity::Math) newProblem();
  if (active_) show(r, now);
}

void OfflineTutor::tick(Robot& r, uint32_t now) {
  if (active_ && reached(now, nextAt_)) next(r, now);
}

void OfflineTutor::show(Robot& r, uint32_t now) {
  uint32_t hold = kLetterMs;
  switch (act_) {
    case Activity::AlphabetEn: {
      const offline::LetterCard& c = offline::kEnAlphabet[index_ % offline::kEnLetters];
      r.showContent(ContentKind::Letter, c.letter, c.caption, false, now, 0);
      break;
    }
    case Activity::AlphabetAr: {
      const offline::LetterCard& c = offline::kArAlphabet[index_ % offline::kArLetters];
      r.showContent(ContentKind::Letter, c.letter, c.caption, true, now, 0);
      break;
    }
    case Activity::Counting: {
      int n = 1 + (index_ % 100);
      snprintf(value_, sizeof(value_), "%d", n);
      r.showContent(ContentKind::Number, value_, offline::kNumberWords[n], false, now, 0);
      hold = kCountMs;
      break;
    }
    case Activity::Math: {
      if (revealed_) snprintf(value_, sizeof(value_), "%d %c %d = %d", a_, op_, b_, answer());
      else snprintf(value_, sizeof(value_), "%d %c %d = ?", a_, op_, b_);
      r.showContent(ContentKind::Math, value_, "", false, now, 0);
      hold = revealed_ ? kAnswerMs : kQuestionMs;
      if (revealed_) r.setExpressionOverride(Expression::Happy, now, 1500);
      break;
    }
    default:
      break;
  }
  nextAt_ = now + hold;
}

}  // namespace robot
