"""Child-safety checks applied to every turn, independent of the AI provider.

This is a deterministic baseline, not a complete moderation system:
  * input  : topics a children's robot must not engage with are redirected; signs of
             distress get a caring answer pointing to a trusted adult; personal data
             (phone numbers, addresses, emails) is never echoed or sent to the model.
  * output : model replies are checked against the same lists and replaced if they hit.
The language model additionally runs under a strict child-safety system prompt, and
providers apply their own safety systems. Review and extend these lists for your audience.
"""
from __future__ import annotations

import re
from dataclasses import dataclass

from .textutil import normalise_answer

BLOCKED = {
    "violence": ["kill", "murder", "gun", "shoot", "bomb", "knife", "weapon", "blood", "stab",
                 "قتل", "اقتل", "مسدس", "سلاح", "قنبلة", "سكين", "دم", "اطعن"],
    "adult": ["sex", "sexy", "naked", "porn", "nude", "جنس", "عاري", "عارية", "اباحي", "إباحي"],
    "drugs": ["drugs", "cocaine", "weed", "alcohol", "beer", "vodka", "cigarette", "vape",
              "مخدرات", "حشيش", "كحول", "خمر", "سجائر", "سيجارة"],
    "hate": ["stupid race", "hate them all", "عنصري"],
}
DISTRESS = ["kill myself", "hurt myself", "want to die", "nobody loves me", "someone hurt me",
            "hits me", "touches me", "اقتل نفسي", "أؤذي نفسي", "اؤذي نفسي", "أريد أن أموت", "اريد ان اموت",
            "لا أحد يحبني", "لا احد يحبني", "يضربني", "يؤذيني"]

_PHONE = re.compile(r"(?:\+?\d[\d\s\-]{7,}\d)")
_EMAIL = re.compile(r"\b[\w.+-]+@[\w-]+\.[\w.]+\b")
_ADDRESS = re.compile(r"\b(?:i live at|my address is|my address|عنواني|اسكن في|أسكن في)\b", re.I)


@dataclass
class Verdict:
    ok: bool
    category: str | None = None
    reply_en: str = ""
    reply_ar: str = ""

    def reply(self, lang: str) -> str:
        return self.reply_ar if lang == "ar" else self.reply_en


def _hits(text: str, words: list[str]) -> bool:
    t = f" {normalise_answer(text)} "
    return any(f" {normalise_answer(w)} " in t for w in words)


def check_input(text: str) -> Verdict:
    if _hits(text, DISTRESS):
        return Verdict(False, "distress",
                       "Thank you for telling me. That sounds really hard. Please tell a grown-up you trust, "
                       "like your mom, dad, or teacher, right now. You are important and you deserve help.",
                       "شكرا لأنك أخبرتني. هذا يبدو صعبا جدا. أرجوك أخبر شخصا كبيرا تثق به، مثل أمك أو أبيك أو معلمك، "
                       "الآن. أنت مهم وتستحق المساعدة.")
    if _PHONE.search(text) or _EMAIL.search(text) or _ADDRESS.search(text):
        return Verdict(False, "personal_info",
                       "Let's keep things like phone numbers and addresses private. Only share them with "
                       "grown-ups you trust! What would you like to learn?",
                       "لنحافظ على أرقام الهواتف والعناوين سرا، ونشاركها فقط مع الكبار الذين نثق بهم! ماذا تحب أن تتعلم؟")
    for cat, words in BLOCKED.items():
        if _hits(text, words):
            return Verdict(False, cat,
                           "That's not something I can talk about. Let's learn something fun instead! "
                           "Do you want a story or a math game?",
                           "هذا موضوع لا أستطيع الحديث عنه. هيا نتعلم شيئا ممتعا! هل تريد قصة أم لعبة حساب؟")
    return Verdict(True)


def check_output(text: str) -> Verdict:
    for cat, words in BLOCKED.items():
        if _hits(text, words):
            return Verdict(False, cat,
                           "Let's talk about something else! Would you like to hear a story?",
                           "لنتحدث عن شيء آخر! هل تحب أن تسمع قصة؟")
    if _EMAIL.search(text) or re.search(r"https?://", text):
        return Verdict(False, "link", "Let's keep learning together! What should we do next?",
                       "هيا نكمل التعلم معا! ماذا نفعل الآن؟")
    return Verdict(True)


def redact_personal(text: str) -> str:
    """Remove personal identifiers before text is sent to any external model."""
    text = _PHONE.sub("[number]", text)
    return _EMAIL.sub("[email]", text)
