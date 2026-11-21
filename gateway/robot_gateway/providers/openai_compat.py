"""OpenAI-compatible REST providers (raw HTTP, so they work with any compatible service).

  OpenAICompatibleSTT  POST {base}/audio/transcriptions   multipart WAV -> {"text": ...}
                       Presets: Groq (Whisper Large v3 Turbo) and OpenAI. Arabic + English.
  OpenAITTS            POST {base}/audio/speech            {"response_format": "pcm"} -> 24 kHz s16le
  OpenAICompatibleLLM  POST {base}/chat/completions        Groq (default openai/gpt-oss-120b), ChatGPT, ...

Keys come from environment variables (STT_API_KEY, or GROQ_API_KEY / OPENAI_API_KEY) and are
never logged. Failures are logged with provider, model, HTTP status and the provider's error
type/code only — never the key, the request text or the provider's free-text message.
Model names are configuration because providers rename and retire models.
These adapters are tested against a mock HTTP transport, not against the live services.
"""
from __future__ import annotations

import io
import logging
import os
import time
import wave
from typing import Callable

import httpx

from .base import ChatMessage, LlmResult, ProviderError, Speech, Transcript

log = logging.getLogger("gateway.provider")

GROQ_BASE_URL = "https://api.groq.com/openai/v1"

# name -> (base URL, default model, key variable, response format)
STT_PRESETS = {
    "groq": (GROQ_BASE_URL, "whisper-large-v3-turbo", "GROQ_API_KEY", "verbose_json"),
    "openai": ("https://api.openai.com/v1", "", "OPENAI_API_KEY", "json"),
}

MAX_RETRY_WAIT_S = 2.0  # a child is waiting: only retry when the provider asks for a short pause


def _client(base_url: str, key: str | None = None, transport: httpx.BaseTransport | None = None) -> httpx.Client:
    key = key if key is not None else os.environ.get("OPENAI_API_KEY", "")
    if not key:
        raise ProviderError("api_key_missing", retryable=False)
    return httpx.Client(base_url=base_url, headers={"Authorization": f"Bearer {key}"}, timeout=25.0,
                        transport=transport)


def _retry_after(r: httpx.Response) -> float | None:
    try:
        return max(0.0, float(r.headers.get("retry-after", "")))
    except ValueError:
        return None


def _error_info(r: httpx.Response) -> tuple[str, str]:
    """The provider's error type and code (short identifiers, never the free-text message)."""
    try:
        err = r.json().get("error") or {}
    except (ValueError, AttributeError):
        err = {}
    if not isinstance(err, dict):
        return "", ""
    return str(err.get("type") or "")[:40], str(err.get("code") or "")[:40]


def _check(r: httpx.Response, what: str, provider: str = "", model: str = "") -> None:
    if r.status_code < 400:
        return
    etype, ecode = _error_info(r)
    if r.status_code in (401, 403):
        code, retryable = f"{what}_auth_failed", False
    elif r.status_code == 429:
        code, retryable = f"{what}_rate_limited", True
    elif "terms" in ecode:
        code, retryable = f"{what}_terms_required", False  # accept the model's terms in the provider console
    elif r.status_code == 404:
        code, retryable = f"{what}_model_not_found", False
    elif r.status_code == 413:
        code, retryable = f"{what}_too_large", False
    else:
        code, retryable = f"{what}_http_{r.status_code}", r.status_code >= 500
    log.warning("provider error: provider=%s what=%s model=%s status=%d type=%s code=%s retry_after=%s -> %s",
                provider or "?", what, model or "?", r.status_code, etype or "-", ecode or "-",
                r.headers.get("retry-after", "-"), code)
    raise ProviderError(code, retryable)


def _send(call: Callable[[], httpx.Response], what: str, provider: str, model: str) -> httpx.Response:
    """One request with at most one quick retry (5xx, or 429 with a short Retry-After). Never waits long:
    the robot gives up after 25 s, and the free plan's limits are better respected than hammered."""
    for attempt in (1, 2):
        try:
            r = call()
        except httpx.TimeoutException as exc:
            log.warning("provider timeout: provider=%s what=%s model=%s", provider, what, model)
            raise ProviderError(f"{what}_timeout") from exc
        except httpx.HTTPError as exc:
            log.warning("provider unreachable: provider=%s what=%s model=%s", provider, what, model)
            raise ProviderError(f"{what}_unreachable") from exc
        if attempt == 1:
            wait = _retry_after(r)
            if r.status_code in (500, 502, 503) or (r.status_code == 429 and wait is not None
                                                   and wait <= MAX_RETRY_WAIT_S):
                log.info("provider retry: provider=%s what=%s model=%s status=%d", provider, what, model,
                         r.status_code)
                time.sleep(min(wait if wait is not None else 0.5, MAX_RETRY_WAIT_S))
                continue
        return r
    return r


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
        with _client(self.base_url, self.key, self.transport) as c:
            r = _send(lambda: c.post("/audio/transcriptions", data=data,
                                     files={"file": ("utterance.wav", buf.getvalue(), "audio/wav")}),
                      "stt", self.name, self.model)
        _check(r, "stt", self.name, self.model)
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
        _check(r, "tts", self.name, self.model)
        return Speech(r.content, self.rate)


class OpenAICompatibleLLM:
    """Chat model over the OpenAI-compatible chat-completions API.

    Presets (see factory.py): groq -> openai/gpt-oss-120b on Groq (free plan available),
    openai -> ChatGPT. `params` are sent as-is (e.g. max_completion_tokens, temperature).
    GPT-OSS reasoning models get reasoning_effort and include_reasoning=false, so only the
    final answer comes back (Groq: reasoning_format is not supported for GPT-OSS).
    """

    def __init__(self, base_url: str, model: str, api_key: str, name: str = "openai", *,
                 params: dict | None = None, reasoning_effort: str = "",
                 transport: httpx.BaseTransport | None = None, timeout: float = 12.0) -> None:
        if not model:
            raise ProviderError(f"{name.upper()} LLM model not set", retryable=False)
        if not api_key:
            raise ProviderError(f"{name.upper()} API key missing for the language model", retryable=False)
        self.base_url, self.model, self.key, self.name = base_url.rstrip("/"), model, api_key, name
        self.params = dict(params or {})
        if model.startswith("openai/gpt-oss"):
            self.params["include_reasoning"] = False
            if reasoning_effort:
                self.params["reasoning_effort"] = reasoning_effort
        self.transport, self.timeout = transport, timeout

    def reply(self, system: str, history: list[ChatMessage], user_text: str) -> LlmResult:
        messages = [{"role": "system", "content": system}]
        messages += [{"role": m.role, "content": m.text} for m in history]
        messages.append({"role": "user", "content": user_text})
        body = {"model": self.model, "messages": messages, **self.params}
        with httpx.Client(base_url=self.base_url, headers={"Authorization": f"Bearer {self.key}"},
                          timeout=self.timeout, transport=self.transport) as c:
            r = _send(lambda: c.post("/chat/completions", json=body), "llm", self.name, self.model)
        _check(r, "llm", self.name, self.model)
        try:
            data = r.json()
            choice = data["choices"][0]
            msg = choice.get("message") or {}
        except (ValueError, KeyError, IndexError, TypeError) as exc:
            log.warning("provider bad response: provider=%s what=llm model=%s", self.name, self.model)
            raise ProviderError("llm_bad_response") from exc
        usage = data.get("usage") or {}
        finish = choice.get("finish_reason")
        refused = bool(msg.get("refusal")) or finish == "content_filter"
        return LlmResult(text="" if refused else (msg.get("content") or "").strip(), refused=refused,
                         truncated=finish == "length", model=str(data.get("model") or self.model),
                         input_tokens=int(usage.get("prompt_tokens") or 0),
                         output_tokens=int(usage.get("completion_tokens") or 0))


class OpenAILLM(OpenAICompatibleLLM):
    """ChatGPT (or any OpenAI-compatible server at OPENAI_BASE_URL) with OPENAI_API_KEY."""

    def __init__(self, base_url: str, model: str) -> None:
        if not model:
            raise ProviderError("OPENAI_LLM_MODEL not set", retryable=False)
        super().__init__(base_url, model, os.environ.get("OPENAI_API_KEY", ""), "openai")
