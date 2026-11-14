"""OpenAI-compatible REST providers (raw HTTP, so they also work with compatible servers).

  OpenAISTT  POST {base}/audio/transcriptions   multipart WAV upload -> {"text": ...}
  OpenAITTS  POST {base}/audio/speech            {"response_format": "pcm"} -> 24 kHz s16le mono
  OpenAILLM  POST {base}/chat/completions        for ChatGPT or another compatible chat model

The key is read from OPENAI_API_KEY. Model names are configuration (OPENAI_*_MODEL) because
they change over time; check your provider's current models and Arabic support.
These adapters are implemented against the published REST shapes but have NOT been exercised
against the live service in this repository's tests.
"""
from __future__ import annotations

import io
import os
import wave

import httpx

from .base import ChatMessage, LlmResult, ProviderError, Speech, Transcript


def _client(base_url: str) -> httpx.Client:
    key = os.environ.get("OPENAI_API_KEY", "")
    if not key:
        raise ProviderError("openai_key_missing", retryable=False)
    return httpx.Client(base_url=base_url, headers={"Authorization": f"Bearer {key}"}, timeout=25.0)


def _check(r: httpx.Response, what: str) -> None:
    if r.status_code in (401, 403):
        raise ProviderError(f"{what}_auth_failed", retryable=False)
    if r.status_code == 429:
        raise ProviderError(f"{what}_rate_limited")
    if r.status_code >= 400:
        raise ProviderError(f"{what}_http_{r.status_code}", retryable=r.status_code >= 500)


class OpenAISTT:
    name = "openai"

    def __init__(self, base_url: str, model: str) -> None:
        if not model:
            raise ProviderError("OPENAI_STT_MODEL not set", retryable=False)
        self.base_url, self.model = base_url, model

    def transcribe(self, pcm: bytes, sample_rate: int, language_hint: str | None) -> Transcript:
        buf = io.BytesIO()
        with wave.open(buf, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(sample_rate)
            w.writeframes(pcm)
        data = {"model": self.model, "response_format": "json"}
        if language_hint in ("en", "ar"):
            data["language"] = language_hint
        try:
            with _client(self.base_url) as c:
                r = c.post("/audio/transcriptions", data=data,
                           files={"file": ("utterance.wav", buf.getvalue(), "audio/wav")})
        except httpx.HTTPError as exc:
            raise ProviderError("stt_unreachable") from exc
        _check(r, "stt")
        body = r.json()
        return Transcript(text=(body.get("text") or "").strip(), language=body.get("language"))


class OpenAITTS:
    name = "openai"
    rate = 24000  # the "pcm" response format is 24 kHz signed 16-bit little-endian mono

    def __init__(self, base_url: str, model: str, voice: str) -> None:
        if not model:
            raise ProviderError("OPENAI_TTS_MODEL not set", retryable=False)
        self.base_url, self.model, self.voice = base_url, model, voice

    def synthesize(self, text: str, language: str) -> Speech:
        try:
            with _client(self.base_url) as c:
                r = c.post("/audio/speech", json={"model": self.model, "voice": self.voice,
                                                  "input": text, "response_format": "pcm"})
        except httpx.HTTPError as exc:
            raise ProviderError("tts_unreachable") from exc
        _check(r, "tts")
        return Speech(r.content, self.rate)


class OpenAILLM:
    name = "openai"

    def __init__(self, base_url: str, model: str) -> None:
        if not model:
            raise ProviderError("OPENAI_LLM_MODEL not set", retryable=False)
        self.base_url, self.model = base_url, model

    def reply(self, system: str, history: list[ChatMessage], user_text: str) -> LlmResult:
        messages = [{"role": "system", "content": system}]
        messages += [{"role": m.role, "content": m.text} for m in history]
        messages.append({"role": "user", "content": user_text})
        try:
            with _client(self.base_url) as c:
                r = c.post("/chat/completions", json={"model": self.model, "messages": messages})
        except httpx.HTTPError as exc:
            raise ProviderError("llm_unreachable") from exc
        _check(r, "llm")
        choice = r.json()["choices"][0]
        return LlmResult(text=(choice["message"].get("content") or "").strip(),
                         truncated=choice.get("finish_reason") == "length", model=self.model)
