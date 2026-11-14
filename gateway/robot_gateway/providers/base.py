"""Provider interfaces. Speech-to-text, language model and text-to-speech are separate,
replaceable components; the pipeline only ever talks to these protocols."""
from __future__ import annotations

from dataclasses import dataclass
from typing import Literal, Protocol


class ProviderError(Exception):
    """A provider failed. `retryable` tells the robot whether trying again may help."""

    def __init__(self, code: str, retryable: bool = True) -> None:
        super().__init__(code)
        self.code = code
        self.retryable = retryable


@dataclass
class Transcript:
    text: str
    language: str | None = None  # "en" / "ar" when the provider reports it


@dataclass
class ChatMessage:
    role: Literal["user", "assistant"]
    text: str


@dataclass
class LlmResult:
    text: str
    refused: bool = False
    truncated: bool = False
    model: str = ""


@dataclass
class Speech:
    pcm: bytes          # signed 16-bit little-endian mono
    sample_rate: int


class SpeechToText(Protocol):
    name: str

    def transcribe(self, pcm: bytes, sample_rate: int, language_hint: str | None) -> Transcript: ...


class LanguageModel(Protocol):
    name: str

    def reply(self, system: str, history: list[ChatMessage], user_text: str) -> LlmResult: ...


class TextToSpeech(Protocol):
    name: str

    def synthesize(self, text: str, language: str) -> Speech: ...
