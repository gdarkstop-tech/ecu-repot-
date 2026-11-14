"""OpenAI-compatible REST providers (raw HTTP, so they work with any compatible service).

  OpenAICompatibleSTT  POST {base}/audio/transcriptions   multipart WAV -> {"text": ...}
                       Presets: Groq (Whisper Large v3 Turbo) and OpenAI. Arabic + English.
  OpenAITTS            POST {base}/audio/speech            {"response_format": "pcm"} -> 24 kHz s16le
  OpenAILLM            POST {base}/chat/completions        ChatGPT or another compatible chat model

Keys come from environment variables (STT_API_KEY, or GROQ_API_KEY / OPENAI_API_KEY) and are
never logged. Model names are configuration because providers rename and retire models.
These adapters are tested against a mock HTTP transport, not against the live services.
"""
from __future__ import annotations

import io
import os
import wave

import httpx

from .base import ChatMessage, LlmResult, ProviderError, Speech, Transcript

# name -> (base URL, default model, key variable, response format)
STT_PRESETS = {
    "groq": ("https://api.groq.com/openai/v1", "whisper-large-v3-turbo", "GROQ_API_KEY", "verbose_json"),
    "openai": ("https://api.openai.com/v1", "", "OPENAI_API_KEY", "json"),
}


def _client(base_url: str, key: str | None = None, transport: httpx.BaseTransport | None = None) -> httpx.Client:
    key = key if key is not None else os.environ.get("OPENAI_API_KEY", "")
    if not key:
        raise ProviderError("api_key_missing", retryable=False)
    return httpx.Client(base_url=base_url, headers={"Authorization": f"Bearer {key}"}, timeout=25.0,
                        transport=transport)


def _check(r: httpx.Response, what: str) -> None:
    if r.status_code in (401, 403):
        raise ProviderError(f"{what}_auth_failed", retryable=False)
    if r.status_code == 429:
        raise ProviderError(f"{what}_rate_limited")
    if r.status_code >= 400:
        raise ProviderError(f"{what}_http_{r.status_code}", retryable=r.status_code >= 500)


class OpenAICompatibleSTT:
    """Speech-to-text over the OpenAI-compatible transcription endpoint."""

    def __init__(self, base_url: str, model: str, api_key: str, response_format: str = "json",
                 name: str = "openai", transport: httpx.BaseTransport | None = None) -> None:
        if not model:
            raise ProviderError("STT_MODEL not set", retryable=False)
        if not api_key:
            raise ProviderError("STT API key missing (STT_API_KEY / GROQ_API_KEY / OPENAI_API_KEY)", retryable=False)
        self.base_url, self.model, self.key = base_url.rstrip("/"), model, api_key
        self.response_format, self.name, self.transport = response_format, name, transport

    def transcribe(self, pcm: bytes, sample_rate: int, language_hint: str | None) -> Transcript:
        buf = io.BytesIO()
        with wave.open(buf, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(sample_rate)
            w.writeframes(pcm)
        data = {"model": self.model, "response_format": self.response_format, "temperature": "0"}
        if language_hint in ("en", "ar"):
            data["language"] = language_hint  # forcing the language improves short-utterance accuracy
        try:
            with _client(self.base_url, self.key, self.transport) as c:
                r = c.post("/audio/transcriptions", data=data,
                           files={"file": ("utterance.wav", buf.getvalue(), "audio/wav")})
        except httpx.HTTPError as exc:
            raise ProviderError("stt_unreachable") from exc
        _check(r, "stt")
        body = r.json()
        text = (body.get("text") or "").strip()
        segments = body.get("segments")
        if isinstance(segments, list) and segments:
            # Whisper's own "this was not speech" signal: drop segments it marks as silence.
            kept = [sg for sg in segments
                    if not (float(sg.get("no_speech_prob", 0)) > 0.6 and float(sg.get("avg_logprob", 0)) < -1.0)]
            if not kept:
                text = ""
            elif len(kept) != len(segments):
                text = " ".join(str(sg.get("text", "")).strip() for sg in kept).strip()
        return Transcript(text=text, language=body.get("language"))


OpenAISTT = OpenAICompatibleSTT  # backward-compatible name


class OpenAITTS:
    name = "openai"
    rate = 24000  # the "pcm" response format is 24 kHz signed 16-bit little-endian mono

    def __init__(self, base_url: str, model: str, voice: str) -> None:
        if not model:
            raise ProviderError("OPENAI_TTS_MODEL not set", retryable=False)
        self.base_url, self.model, self.voice = base_url, model, voice

    def synthesize(self, text: str, language: str) -> Speech:
        try:
            with _client(self.base_url, os.environ.get("OPENAI_API_KEY", "")) as c:
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
            with _client(self.base_url, os.environ.get("OPENAI_API_KEY", "")) as c:
                r = c.post("/chat/completions", json={"model": self.model, "messages": messages})
        except httpx.HTTPError as exc:
            raise ProviderError("llm_unreachable") from exc
        _check(r, "llm")
        choice = r.json()["choices"][0]
        return LlmResult(text=(choice["message"].get("content") or "").strip(),
                         truncated=choice.get("finish_reason") == "length", model=self.model)
