"""Keyless providers for development and demos. They never pretend to understand speech:
the mock STT reports how long the child spoke, and replies say demo mode is active."""
from __future__ import annotations

import array
import math

from .base import ChatMessage, LlmResult, Speech, Transcript

DEMO_PREFIX = "[demo-audio "


class MockSTT:
    name = "mock"

    def transcribe(self, pcm: bytes, sample_rate: int, language_hint: str | None) -> Transcript:
        samples = array.array("h")
        samples.frombytes(pcm[: len(pcm) - (len(pcm) % 2)])
        frame = max(1, sample_rate // 50)  # 20 ms
        voiced = 0
        for i in range(0, len(samples) - frame + 1, frame):
            chunk = samples[i : i + frame]
            rms = math.sqrt(sum(s * s for s in chunk) / frame)
            if rms > 500:  # ~ -36 dBFS: clearly louder than room noise
                voiced += 1
        seconds = voiced * 0.02
        if seconds < 0.3:
            return Transcript("", language_hint)
        return Transcript(f"{DEMO_PREFIX}{seconds:.1f}s]", language_hint if language_hint in ("en", "ar") else "en")


class MockLLM:
    """Only used for questions the tutor does not handle; the pipeline supplies canned answers."""

    name = "mock"

    def reply(self, system: str, history: list[ChatMessage], user_text: str) -> LlmResult:
        return LlmResult(text="", model="mock")


class MockTTS:
    """Not speech: a short two-tone chime whose length follows the text, so the speaker,
    amplifier and streaming path can be tested without any TTS service."""

    name = "mock"
    rate = 16000

    def synthesize(self, text: str, language: str) -> Speech:
        seconds = min(4.0, 0.6 + 0.03 * len(text))
        n = int(self.rate * seconds)
        out = array.array("h")
        for i in range(n):
            t = i / self.rate
            f = 660.0 if int(t * 4) % 2 == 0 else 880.0
            env = min(1.0, i / 400, (n - i) / 400)
            out.append(int(6000 * env * math.sin(2 * math.pi * f * t)))
        return Speech(out.tobytes(), self.rate)
