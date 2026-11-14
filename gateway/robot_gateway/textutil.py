"""Language detection, Arabic normalisation, and OLED-ready text (shaped, visual order)."""
from __future__ import annotations

import re

import arabic_reshaper
from bidi.algorithm import get_display

_ARABIC = re.compile(r"[؀-ۿݐ-ݿﭐ-﷿ﹰ-﻿]")
_DIACRITICS = re.compile(r"[ؐ-ًؚ-ٰٟۖ-ۭ]")

# The robot's OLED text field holds 160 bytes; Arabic presentation forms are 3 bytes each.
OLED_MAX_BYTES = 150


def detect_language(text: str) -> str:
    letters = [c for c in text if c.isalpha()]
    if not letters:
        return "en"
    arabic = sum(1 for c in letters if _ARABIC.match(c))
    return "ar" if arabic * 2 >= len(letters) else "en"


def normalise_answer(text: str) -> str:
    """Lower-case, strip punctuation/diacritics, unify Arabic letter variants."""
    t = _DIACRITICS.sub("", text.casefold())
    t = t.translate(str.maketrans({"أ": "ا", "إ": "ا", "آ": "ا", "ة": "ه", "ى": "ي", "ـ": ""}))
    t = re.sub(r"[^\w\s]", " ", t)
    return re.sub(r"\s+", " ", t).strip()


def contains_answer(text: str, accepted: set[str]) -> bool:
    words = f" {normalise_answer(text)} "
    for a in accepted:
        na = normalise_answer(a)
        if na and (f" {na} " in words or f" ال{na} " in words):
            return True
    return False


def first_sentence(text: str, max_chars: int) -> str:
    text = re.sub(r"\s+", " ", text).strip()
    m = re.match(r"(.+?[.!?؟])(\s|$)", text)
    s = m.group(1) if m else text
    if len(s) <= max_chars:
        return s
    cut = s[:max_chars].rsplit(" ", 1)[0]
    return cut + "..."


def to_oled(text: str) -> tuple[str, bool]:
    """Return (display_text, rtl). Arabic is reshaped and reordered for a display without
    a shaping engine, and trimmed to fit the robot's buffer without splitting characters."""
    text = re.sub(r"\s+", " ", text).strip()
    if detect_language(text) != "ar":
        out = text.encode("utf-8")[:OLED_MAX_BYTES].decode("utf-8", "ignore")
        return out, False
    logical = text
    while True:
        visual = get_display(arabic_reshaper.reshape(logical))
        if len(visual.encode("utf-8")) <= OLED_MAX_BYTES or len(logical) < 4:
            return visual, True
        logical = logical[: int(len(logical) * 0.85)].rsplit(" ", 1)[0]


def strip_for_speech(text: str) -> str:
    """Remove markdown, URLs and emoji: replies are spoken aloud and shown on a tiny screen."""
    t = re.sub(r"https?://\S+", "", text)
    t = re.sub(r"[*_`#>|~]+", "", t)
    t = re.sub(r"[\U0001F000-\U0001FAFF☀-➿️]", "", t)
    t = re.sub(r"^\s*[-•]\s*", "", t, flags=re.M)
    return re.sub(r"\s+", " ", t).strip()
