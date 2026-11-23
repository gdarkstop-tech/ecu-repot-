"""Educational tutor: learning modes, questions, answer checking, and difficulty by age.

Everything here is deterministic. The language model is only consulted for open questions
that the tutor does not handle; arithmetic is always computed by mathcheck.
"""
from __future__ import annotations

import random
import time
from collections import deque
from dataclasses import dataclass, field
from fractions import Fraction

from ..textutil import contains_answer, normalise_answer
from . import content as C
from .mathcheck import extract_number, find_arithmetic, format_number


@dataclass
class Content:
    kind: str           # letter | number | math | word
    value: str
    caption: str = ""


@dataclass
class TutorReply:
    text: str
    lang: str
    emotion: str = "happy"      # happy | sad | surprised | idle | thinking
    oled: str | None = None     # short subtitle (logical order; shaped later)
    content: Content | None = None
    source: str = "tutor"


@dataclass
class Pending:
    expected_number: Fraction | None = None
    accepted: set[str] = field(default_factory=set)
    answer_text: str = ""
    attempts: int = 0
    nudged: bool = False
    content_answer: Content | None = None
    # Practice drills never move on until the child gets it right:
    #   "say"   repeat-after-me (alphabet, vocabulary), "count" say the next number.
    drill: str = ""


@dataclass
class Session:
    device_id: str
    mode: str = "chat"
    index: int = 0
    pending: Pending | None = None
    level: int = 1
    streak: int = 0
    misses: int = 0
    quiz_asked: int = 0
    quiz_correct: int = 0
    history: deque = field(default_factory=lambda: deque(maxlen=16))
    last_seen: float = field(default_factory=time.monotonic)
    rng: random.Random = field(default_factory=random.Random)


MODE_KEYWORDS = {
    "alphabet": ["alphabet", "letters", "letter", "abc", "الحروف", "حروف", "الأبجدية", "الابجدية"],
    "counting": ["count", "counting", "عد", "العد", "نعد"],
    "math": ["math", "maths", "sums", "adding", "رياضيات", "الرياضيات", "حساب", "الحساب", "جمع"],
    "vocab_en": ["english words", "english word", "vocabulary", "كلمات انجليزية", "كلمات إنجليزية",
                 "الانجليزية", "الإنجليزية", "انجليزي"],
    "vocab_ar": ["arabic words", "arabic word", "in arabic", "كلمات عربية", "العربية", "عربي"],
    "quiz": ["quiz", "test me", "ask me", "اختبار", "اختبرني", "مسابقة", "اسألني"],
    "story": ["story", "tale", "قصة", "حكاية", "قصه"],
    "wisdom": ["lesson", "wisdom", "advice", "حكمة", "نصيحة", "درس"],
    "vision": ["what is this", "what do you see", "look at", "can you see", "ماذا ترى", "شو هذا",
               "ما هذا", "انظر"],
    "chat": ["stop", "let's talk", "free talk", "توقف", "خلص", "لنتحدث"],
}
DONT_KNOW = ["don't know", "dont know", "no idea", "not sure", "skip", "pass", "tell me", "لا أعرف", "لا اعرف",
             "ما بعرف", "مش عارف", "ما اعرف", "تخطى", "قل لي"]
NEXT_WORDS = ["next", "more", "again", "another", "continue", "yes", "التالي", "كمان", "مرة أخرى",
              "أخرى", "نعم", "ايوه", "أيوه", "أكمل", "اكمل"]


def _say(lang: str, en: str, ar: str) -> str:
    return ar if lang == "ar" else en


def _match_mode(text: str) -> str | None:
    t = f" {normalise_answer(text)} "
    for mode, words in MODE_KEYWORDS.items():
        for w in words:
            if f" {normalise_answer(w)} " in t:
                return mode
    return None


def level_for_age(age: int) -> int:
    return 0 if age <= 5 else (1 if age <= 7 else 2)


class Tutor:
    def __init__(self, robot_name: str = "Robo") -> None:
        self.robot_name = robot_name

    # ------------------------------------------------------------------ entry point
    def handle(self, s: Session, text: str, lang: str, age: int) -> TutorReply | None:
        mode = _match_mode(text)
        if mode == "vision":
            return TutorReply(
                _say(lang, "I can't see yet. My camera is not connected, but soon I will learn to look "
                           "at things with you!",
                     "لا أستطيع أن أرى الآن، فالكاميرا غير موصولة بعد. قريبا سأتعلم أن أرى الأشياء معك!"),
                lang, emotion="sad", oled=_say(lang, "Camera not connected yet", "الكاميرا غير موصولة بعد"),
            )
        if mode and mode == s.mode and s.pending is not None and s.pending.drill:
            # "Letters again!" during practice: repeat the current item (in this language), no penalty.
            return self.present(s, lang, age)
        if mode and mode != s.mode or (mode and mode in ("story", "wisdom", "quiz")):
            return self.start_mode(s, mode, lang, age)

        if s.pending is not None:
            return self.check_answer(s, text, lang)

        ev = find_arithmetic(text)
        if ev is not None:
            return self.math_answer(ev, lang)

        if s.mode in ("alphabet", "vocab_en", "vocab_ar", "counting"):
            return self.present(s, lang, age)  # a drill without a pending item (e.g. language switch)
        if s.mode in ("story", "wisdom") and any(w in text.lower() for w in NEXT_WORDS):
            s.index += 1
            return self.present(s, lang, age)
        return None

    # ------------------------------------------------------------------ modes
    def start_mode(self, s: Session, mode: str, lang: str, age: int) -> TutorReply:
        if s.mode != mode:
            s.index = 0
        else:
            s.index += 1  # "another story" etc.
        s.mode = mode
        s.pending = None
        s.level = level_for_age(age)
        s.streak = s.misses = 0
        if mode == "quiz":
            s.quiz_asked = s.quiz_correct = 0
        if mode == "chat":
            return TutorReply(_say(lang, "Okay! Ask me anything.", "حسنا! اسألني أي شيء."), lang)
        return self.present(s, lang, age)

    def present(self, s: Session, lang: str, age: int) -> TutorReply:
        if s.mode == "alphabet":
            if lang == "ar":
                letter, name, word, _ = C.AR_ALPHABET[s.index % len(C.AR_ALPHABET)]
                s.pending = Pending(accepted={word}, answer_text=word, drill="say")
                return TutorReply(f"هذا حرف ال{name}. {letter} مثل {word}. قل معي: {word}!", lang,
                                  emotion="happy", oled=f"{letter} مثل {word}", content=Content("letter", letter, word))
            letter, word = C.EN_ALPHABET[s.index % len(C.EN_ALPHABET)]
            s.pending = Pending(accepted={word}, answer_text=word, drill="say")
            return TutorReply(f"This is the letter {letter}. {letter} is for {word}. Can you say {word}?",
                              lang, emotion="happy", oled=f"{letter} is for {word}",
                              content=Content("letter", letter, word))
        if s.mode == "counting":
            # Counting continues in order from the number the child last said (s.index).
            if s.index <= 0 or s.index > 96:
                start = 1 if s.level == 0 else s.rng.randint(1, 20 if s.level == 1 else 90)
            else:
                start = s.index + 1
            seq = list(range(start, start + 3))
            nxt = seq[-1] + 1
            s.pending = Pending(expected_number=Fraction(nxt), answer_text=str(nxt), drill="count",
                                content_answer=Content("number", str(nxt), C.number_word_en(nxt) if nxt <= 100 else ""))
            words = ", ".join(str(n) for n in seq)
            return TutorReply(_say(lang, f"Let's count together! {words}. What number comes next?",
                                   f"هيا نعد معا! {words}. ما الرقم التالي؟"),
                              lang, emotion="happy", oled=f"{words}, ?", content=Content("number", f"{seq[-1]} ?"))
        if s.mode == "math":
            return self.ask_math(s, lang)
        if s.mode == "vocab_en":
            en, ar = C.VOCAB[s.index % len(C.VOCAB)]
            text = _say(lang, f"Our English word is: {en}. Can you say {en}?",
                        f"الكلمة الإنجليزية هي {en}، ومعناها {ar}. قل معي: {en}!")
            s.pending = Pending(accepted={en, ar}, answer_text=en, drill="say")  # the word or its meaning
            return TutorReply(text, lang, oled=f"{en} = {ar}", content=Content("word", en.upper(), ar))
        if s.mode == "vocab_ar":
            en, ar = C.VOCAB[s.index % len(C.VOCAB)]
            text = _say(lang, f"In Arabic, {en} is {ar}. Can you say {ar}?",
                        f"كلمتنا اليوم هي {ar}. قل معي: {ar}!")
            s.pending = Pending(accepted={ar}, answer_text=ar, drill="say")
            return TutorReply(text, lang, oled=f"{ar} = {en}", content=Content("word", ar, en))
        if s.mode == "quiz":
            return self.ask_quiz(s, lang)
        if s.mode == "story":
            stories = C.STORIES["ar" if lang == "ar" else "en"]
            story = stories[s.index % len(stories)]
            tail = _say(lang, " Would you like another story?", " هل تريد قصة أخرى؟")
            return TutorReply(story + tail, lang, oled=_say(lang, "Story time!", "وقت القصة!"))
        if s.mode == "wisdom":
            items = C.WISDOM["ar" if lang == "ar" else "en"]
            return TutorReply(items[s.index % len(items)], lang, oled=items[s.index % len(items)])
        return TutorReply(_say(lang, "Okay! Ask me anything.", "حسنا! اسألني أي شيء."), lang)

    # ------------------------------------------------------------------ math
    def make_problem(self, s: Session) -> tuple[int, str, int, int]:
        r = s.rng
        if s.level <= 0:
            a, b, op = r.randint(0, 5), r.randint(0, 5), "+"
        elif s.level == 1:
            a, b, op = r.randint(0, 10), r.randint(0, 10), r.choice("+-")
        elif s.level == 2:
            a, b, op = r.randint(0, 20), r.randint(0, 20), r.choice("+-")
        else:
            a, b, op = r.randint(2, 10), r.randint(1, 10), "x"
        if op == "-" and b > a:
            a, b = b, a  # no negative answers for children
        ans = a + b if op == "+" else (a - b if op == "-" else a * b)
        return a, op, b, ans

    def ask_math(self, s: Session, lang: str) -> TutorReply:
        a, op, b, ans = self.make_problem(s)
        spoken_op = {"+": ("plus", "زائد"), "-": ("minus", "ناقص"), "x": ("times", "ضرب")}[op]
        s.pending = Pending(expected_number=Fraction(ans), answer_text=str(ans),
                            content_answer=Content("math", f"{a} {op} {b} = {ans}"))
        return TutorReply(_say(lang, f"What is {a} {spoken_op[0]} {b}?", f"كم يساوي {a} {spoken_op[1]} {b}؟"),
                          lang, emotion="thinking", oled=f"{a} {op} {b} = ?", content=Content("math", f"{a} {op} {b} = ?"))

    def math_answer(self, ev, lang: str) -> TutorReply:
        expr = ev.expression
        if ev.value.denominator != 1 and lang != "ar":
            text = f"{expr} equals {ev.answer_text}."
        else:
            text = _say(lang, f"{expr} equals {ev.answer_text}. Great question!",
                        f"{expr} يساوي {ev.answer_text}. سؤال رائع!")
        shown = f"{expr} = {ev.answer_text}".replace("×", "x").replace("÷", "/")
        return TutorReply(text, lang, emotion="happy", oled=shown, content=Content("math", shown), source="math")

    # ------------------------------------------------------------------ quiz
    def ask_quiz(self, s: Session, lang: str) -> TutorReply:
        if s.quiz_asked >= 5:
            score = f"{s.quiz_correct}/5"
            s.mode, s.pending = "chat", None
            return TutorReply(_say(lang, f"Quiz finished! You got {s.quiz_correct} out of 5. Well done!",
                                   f"انتهت المسابقة! حصلت على {s.quiz_correct} من 5. أحسنت!"),
                              lang, emotion="happy", oled=score, content=Content("number", score))
        s.quiz_asked += 1
        kind = s.rng.choice(["math", "general", "vocab"])
        if kind == "math":
            r = self.ask_math(s, lang)
            r.text = _say(lang, f"Question {s.quiz_asked}. ", f"السؤال {s.quiz_asked}. ") + r.text
            return r
        if kind == "vocab":
            en, ar = s.rng.choice(C.VOCAB[:15])
            s.pending = Pending(accepted={ar} if lang != "ar" else {en}, answer_text=ar if lang != "ar" else en)
            q = _say(lang, f"Question {s.quiz_asked}. How do you say {en} in Arabic?",
                     f"السؤال {s.quiz_asked}. كيف نقول {ar} بالإنجليزية؟")
            return TutorReply(q, lang, emotion="thinking", oled=q)
        q_en, q_ar, accepted = s.rng.choice(C.QUIZ)
        nums = {a for a in accepted if a.isdigit()}
        s.pending = Pending(accepted=set(accepted), answer_text=sorted(accepted, key=len)[0],
                            expected_number=Fraction(next(iter(nums))) if nums else None)
        q = _say(lang, f"Question {s.quiz_asked}. {q_en}", f"السؤال {s.quiz_asked}. {q_ar}")
        return TutorReply(q, lang, emotion="thinking", oled=_say(lang, q_en, q_ar))

    # ------------------------------------------------------------------ answers
    def check_answer(self, s: Session, text: str, lang: str) -> TutorReply:
        p = s.pending
        assert p is not None
        if p.drill:
            return self.check_drill(s, text, lang)
        correct = False
        gave_up = any(w in text.lower() for w in DONT_KNOW)
        if p.expected_number is not None:
            n = extract_number(text)
            correct = n is not None and n == p.expected_number
            if n is None and not p.accepted and not gave_up and not p.nudged:
                p.nudged = True  # ask once for a number; a second non-number counts as an attempt
                return TutorReply(_say(lang, "Tell me a number!", "قل لي رقما!"), lang, emotion="thinking")
        if not correct and p.accepted:
            correct = contains_answer(text, p.accepted)
        if not correct and gave_up:
            p.attempts = max(p.attempts, 1)  # "I don't know": show the answer now, no penalty loop

        if correct:
            s.streak += 1
            s.misses = 0
            if s.mode == "quiz":
                s.quiz_correct += 1
            if s.streak >= 3 and s.level < 3:
                s.level += 1
                s.streak = 0
            praise = s.rng.choice([_say(lang, "Correct! Great job!", "صحيح! أحسنت!"),
                                   _say(lang, "Yes! You are so smart!", "نعم! أنت ذكي جدا!"),
                                   _say(lang, "That's right! Well done!", "إجابة صحيحة! ممتاز!")])
            s.pending = None
            nxt = self._next_question(s, lang)
            nxt.text = f"{praise} {nxt.text}"
            nxt.emotion = "happy"
            return nxt

        p.attempts += 1
        if p.attempts < 2:
            return TutorReply(_say(lang, "Not quite. Try again!", "ليس تماما. حاول مرة أخرى!"), lang,
                              emotion="surprised", content=None)
        s.misses += 1
        s.streak = 0
        if s.misses >= 2 and s.level > 0:
            s.level -= 1
            s.misses = 0
        reveal = _say(lang, f"Good try! The answer is {p.answer_text}.", f"محاولة جيدة! الجواب هو {p.answer_text}.")
        shown = p.content_answer
        s.pending = None
        nxt = self._next_question(s, lang)
        nxt.text = f"{reveal} {nxt.text}"
        nxt.emotion = "sad"
        if shown is not None and nxt.content is None:
            nxt.content = shown
        return nxt

    # ------------------------------------------------------------------ practice drills
    def check_drill(self, s: Session, text: str, lang: str) -> TutorReply:
        """Alphabet, counting and vocabulary: never move on until the child says it right.
        An answer from another place in the order is acknowledged, then the child is asked to go
        in order. "I don't know" gets the answer and the same item again."""
        p = s.pending
        gave_up = any(w in text.lower() for w in DONT_KNOW)
        card = self._card(s, lang)
        if p.drill == "count":
            n = extract_number(text)
            expected = p.expected_number
            prev = format_number(expected - 1)
            if n is not None and n == expected:
                s.index = int(expected)
                return self._drill_correct(s, lang)
            p.attempts += 1
            if n is not None and n > expected:
                a, b, c = (format_number(expected - k) for k in (3, 2, 1))
                return TutorReply(_say(lang, f"{format_number(n)} comes later! Let's count in order: {a}, {b}, {c}, "
                                             "and then?", f"{format_number(n)} يأتي لاحقا! لنعد بالترتيب: {a}، {b}، {c}، ثم؟"),
                                  lang, emotion="playful", content=card)
            if n is not None:
                return TutorReply(self._encourage(p, lang, _say(lang, f"No, not {format_number(n)}. Try again! "
                                                                      f"What comes after {prev}?",
                                                                f"لا، ليس {format_number(n)}. حاول مرة أخرى! ما الرقم بعد {prev}؟")),
                                  lang, emotion="worried", content=card)
            if gave_up:
                ans = format_number(expected)
                return TutorReply(_say(lang, f"It's {ans}! Can you say {ans}?", f"إنه {ans}! قل: {ans}!"), lang,
                                  emotion="curious", content=card)
            return TutorReply(_say(lang, f"Tell me a number! What comes after {prev}?", f"قل لي رقما! ما الرقم بعد {prev}؟"),
                              lang, emotion="confused", content=card)

        if contains_answer(text, p.accepted):
            s.index += 1
            return self._drill_correct(s, lang)
        p.attempts += 1
        word = p.answer_text
        if gave_up:
            return TutorReply(_say(lang, f"It's {word}! Can you say {word}?", f"إنها {word}! قل: {word}!"), lang,
                              emotion="curious", content=card)
        other = self._out_of_order(s, text, lang)
        if s.mode == "alphabet":
            if lang == "ar":
                letter, name, _, _ = C.AR_ALPHABET[s.index % len(C.AR_ALPHABET)]
                again = f"هذا حرف ال{name}. {letter} مثل {word}. قل: {word}!"
            else:
                letter = C.EN_ALPHABET[s.index % len(C.EN_ALPHABET)][0]
                again = f"This is {letter}. {letter} is for {word}. Can you say {word}?"
            if other:
                o_letter, o_word = other
                return TutorReply(_say(lang, f"Yes, {o_word} is for {o_letter}! But let's go in order. {again}",
                                       f"صحيح، {o_word} تبدأ بحرف {o_letter}! لكن لنمش بالترتيب. {again}"),
                                  lang, emotion="playful", content=card)
            return TutorReply(self._encourage(p, lang, _say(lang, f"No, that's not it. Listen! {again}",
                                                            f"لا، ليست هذه. اسمع! {again}")),
                              lang, emotion="worried", content=card)
        if other:
            return TutorReply(_say(lang, f"Yes, that's another word we learn! But let's go in order. Can you say {word}?",
                                   f"صحيح، هذه كلمة أخرى نتعلمها! لكن لنمش بالترتيب. قل: {word}!"),
                              lang, emotion="playful", content=card)
        return TutorReply(self._encourage(p, lang, _say(lang, f"No, that's not it. Listen: {word}. Can you say {word}?",
                                                        f"لا، ليست هذه. اسمع: {word}. قل: {word}!")),
                          lang, emotion="worried", content=card)

    def _drill_correct(self, s: Session, lang: str) -> TutorReply:
        s.pending = None
        s.streak += 1
        s.misses = 0
        praise = s.rng.choice([_say(lang, "Yes! Great job!", "نعم! أحسنت!"), _say(lang, "That's right!", "صحيح!"),
                               _say(lang, "Perfect!", "ممتاز!")])
        nxt = self.present(s, lang, 6)
        nxt.text = f"{praise} {nxt.text}"
        nxt.emotion = "excited" if s.streak % 5 == 0 else "proud"
        return nxt

    def _encourage(self, p: Pending, lang: str, text: str) -> str:
        return text + (_say(lang, " You can do it!", " أنت تستطيع!") if p.attempts >= 3 else "")

    def _card(self, s: Session, lang: str) -> Content | None:
        """The current item, so the screen keeps showing it while the child tries again."""
        if s.mode == "alphabet":
            if lang == "ar":
                letter, _, word, _ = C.AR_ALPHABET[s.index % len(C.AR_ALPHABET)]
            else:
                letter, word = C.EN_ALPHABET[s.index % len(C.EN_ALPHABET)]
            return Content("letter", letter, word)
        if s.mode == "counting" and s.pending and s.pending.expected_number is not None:
            return Content("number", f"{format_number(s.pending.expected_number - 1)} ?")
        if s.mode in ("vocab_en", "vocab_ar"):
            en, ar = C.VOCAB[s.index % len(C.VOCAB)]
            return Content("word", en.upper(), ar) if s.mode == "vocab_en" else Content("word", ar, en)
        return None

    def _out_of_order(self, s: Session, text: str, lang: str) -> tuple[str, str] | None:
        """Another item of the same list (e.g. "elephant" while on G): (its letter, its word)."""
        if s.mode == "alphabet":
            items = ([(e[0], e[2]) for e in C.AR_ALPHABET] if lang == "ar" else list(C.EN_ALPHABET))
            n = len(items)
        else:
            items = [(en, en if s.mode == "vocab_en" else ar) for en, ar in C.VOCAB]
            n = len(items)
        cur = s.index % n
        for i, (key, word) in enumerate(items):
            if i != cur and contains_answer(text, {word}):
                return key, word
        return None

    def _next_question(self, s: Session, lang: str) -> TutorReply:
        if s.mode == "math":
            return self.ask_math(s, lang)
        if s.mode == "quiz":
            return self.ask_quiz(s, lang)
        if s.mode == "counting":
            return self.present(s, lang, 6)
        return TutorReply("", lang)

    # ------------------------------------------------------------------ demo answers
    def demo_answer(self, text: str, lang: str) -> TutorReply:
        t = normalise_answer(text)
        for keys, en, ar in C.DEMO_QA:
            if any(normalise_answer(k) in t for k in keys):
                return TutorReply(_say(lang, en, ar).format(name=self.robot_name), lang, source="demo")
        return TutorReply(
            _say(lang, "That's a great question! I'm in demo mode, so I only know a few things. "
                       "Shall we learn letters, numbers, math, or hear a story?",
                 "سؤال رائع! أنا في وضع التجربة ولا أعرف إلا أشياء قليلة. هل نتعلم الحروف أو الأرقام أو الحساب أو نسمع قصة؟"),
            lang, emotion="idle", source="demo")


__all__ = ["Tutor", "Session", "TutorReply", "Content", "format_number"]
