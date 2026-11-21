"""Natural voice with Groq Orpheus text-to-speech (optional: TTS_PROVIDER=groq, uses GROQ_API_KEY).

  POST https://api.groq.com/openai/v1/audio/speech
       {"model", "voice", "input" (<= 200 characters), "response_format": "wav"}

  English: canopylabs/orpheus-v1-english  (voices autumn, diana, hannah, austin, daniel, troy)
  Arabic : canopylabs/orpheus-arabic-saudi (voices abdullah, fahad, sultan, lulwa, noura, aisha)

What the robot gets: signed 16-bit little-endian mono PCM at 24 kHz (a rate the firmware plays),
peak-limited to -1 dBFS so the amplifier never receives a clipped waveform; the robot's own
`volume` setting is applied on top. Bounded: at most GROQ_TTS_MAX_CHUNKS pieces of <= 200
characters per answer (Groq's free plan allows only about 100 speech requests a day), so a long
answer is spoken only in part — the whole answer is still sent as text for the OLED.

Rate limits and errors raise ProviderError (tts_rate_limited, tts_auth_failed, ...); the gateway
then serves the answer as text. It never retries for long and never switches to a paid service.
"""
from __future__ import annotations

import logging
import re
from typing import Iterator

import httpx

from ..audio import decode_wav, limit_peak, resample, silence
from .base import ProviderError, Speech
from .openai_compat import GROQ_BASE_URL, _check, _send

log = logging.getLogger("gateway.provider")

MAX_INPUT_CHARS = 200          # Groq Orpheus input limit per request
MAX_WAV_BYTES = 8_000_000      # refuse absurd responses instead of buffering them
GAP_SECONDS = 0.12             # pause between pieces of one answer

DEFAULT_MODELS = {"en": "canopylabs/orpheus-v1-english", "ar": "canopylabs/orpheus-arabic-saudi"}
DEFAULT_VOICES = {"en": "hannah", "ar": "lulwa"}


def clean_for_speech(text: str) -> str:
    """Remove bracketed text (Orpheus reads [..] as a vocal direction) and leftover markup."""
    t = re.sub(r"\[[^\]]{0,40}\]", " ", text)
    t = re.sub(r"[\[\]<>{}*_#`|~]", " ", t)
    return re.sub(r"\s+", " ", t).strip()


def split_for_speech(text: str, limit: int = MAX_INPUT_CHARS) -> list[str]:
    """Pieces of <= limit characters, cut at a sentence end, else a comma, else a space."""
    pieces: list[str] = []
    rest = text.strip()
    while rest:
        if len(rest) <= limit:
            pieces.append(rest)
            break
        window = rest[: limit + 1]
        cut = -1
        for pattern in (r"[.!?؟…](?=\s)", r"[,،;:](?=\s)", r"\s"):
            hits = [m.end() for m in re.finditer(pattern, window) if m.end() >= limit * 0.35]
            if hits:
                cut = hits[-1]
                break
        if cut <= 0:
            cut = limit
        piece, rest = rest[:cut].strip(), rest[cut:].strip()
        if piece:
            pieces.append(piece)
    return pieces


class GroqOrpheusTTS:
    name = "groq"
    rate = 24000  # what the robot receives (Orpheus' own rate; resampled if Groq ever sends another)

    def __init__(self, api_key: str, *, models: dict[str, str] | None = None, voices: dict[str, str] | None = None,
                 max_chunks: int = 2, base_url: str = GROQ_BASE_URL,
                 transport: httpx.BaseTransport | None = None, timeout: float = 12.0) -> None:
        if not api_key:
            raise ProviderError("GROQ_API_KEY missing for text-to-speech", retryable=False)
        self.key, self.base_url = api_key, base_url.rstrip("/")
        self.models = {**DEFAULT_MODELS, **{k: v for k, v in (models or {}).items() if v}}
        self.voices = {**DEFAULT_VOICES, **{k: v for k, v in (voices or {}).items() if v}}
        self.max_chunks = max(1, min(int(max_chunks), 5))
        self.transport, self.timeout = transport, timeout

    @property
    def model(self) -> str:  # reported by /healthz
        return f"{self.models['en']} / {self.models['ar']}"

    def pieces(self, text: str) -> list[str]:
        return split_for_speech(clean_for_speech(text))[: self.max_chunks]

    def _one(self, client: httpx.Client, piece: str, lang: str) -> bytes:
        model, voice = self.models[lang], self.voices[lang]
        body = {"model": model, "voice": voice, "input": piece, "response_format": "wav"}
        r = _send(lambda: client.post("/audio/speech", json=body), "tts", self.name, model)
        _check(r, "tts", self.name, model)
        if len(r.content) > MAX_WAV_BYTES:
            raise ProviderError("tts_too_large", retryable=False)
        try:
            pcm, rate = decode_wav(r.content)
        except ValueError as exc:
            log.warning("provider bad audio: provider=groq what=tts model=%s (%s)", model, exc)
            raise ProviderError("tts_bad_audio", retryable=False) from exc
        return limit_peak(resample(pcm, rate, self.rate))

    def synthesize_chunks(self, text: str, language: str) -> Iterator[Speech]:
        """Yields one Speech per piece, so playback can start after the first one. An error on the
        first piece propagates; a later one ends the answer early (what was spoken stays spoken)."""
        lang = "ar" if language == "ar" else "en"
        pieces = self.pieces(text)
        with httpx.Client(base_url=self.base_url, headers={"Authorization": f"Bearer {self.key}"},
                          timeout=self.timeout, transport=self.transport) as client:
            for i, piece in enumerate(pieces):
                try:
                    pcm = self._one(client, piece, lang)
                except ProviderError:
                    if i == 0:
                        raise
                    log.warning("tts stopped after %d of %d pieces", i, len(pieces))
                    return
                yield Speech((silence(GAP_SECONDS, self.rate) if i else b"") + pcm, self.rate)

    def synthesize(self, text: str, language: str) -> Speech:
        return Speech(b"".join(s.pcm for s in self.synthesize_chunks(text, language)), self.rate)
