"""How reply speech reaches the robot: POST reply -> audio.path -> GET /v1/turns/{id}/audio.

StoredSpeech (long-running host): speech is synthesised during the turn and kept in memory
    until the robot fetches it once (or AUDIO_TTL_SECONDS pass).
TicketSpeech (serverless host): nothing is kept. The audio path carries a sealed ticket (text,
    language, sample rate, expiry; bound to the device). The GET may reach any instance; it
    opens the ticket and synthesises the speech there. Tickets cannot be one-time without
    shared storage: they are device-bound and expire after AUDIO_TTL_SECONDS instead.

Robots without a speaker never request audio (X-Robot-Output: text), so neither path runs.

Providers that synthesise in pieces (Groq Orpheus: <= 200 characters per request) expose
synthesize_chunks(); TicketSpeech then streams each piece as soon as it is ready, so the robot
starts speaking after the first piece. An error on the first piece is returned as an HTTP error
(429 for a rate limit); a later one just ends the audio early.
"""
from __future__ import annotations

import itertools
import logging
import re
import time
from typing import Iterator

from .providers.base import ProviderError, TextToSpeech
from .sealing import Sealer
from .state import AudioStore

log = logging.getLogger("gateway.speech")

# Vercel Functions reject responses over 4.5 MB; stay below that on every host.
MAX_AUDIO_BYTES = 4_000_000
TICKET_MAX_CHARS = 1000      # the robot accepts audio paths up to 1023 characters
_PATH_ID = re.compile(r"[A-Za-z0-9_-]{8,1000}")


def _bounded(pcm: bytes) -> bytes:
    if len(pcm) <= MAX_AUDIO_BYTES:
        return pcm
    log.warning("reply audio truncated from %d to %d bytes", len(pcm), MAX_AUDIO_BYTES)
    return pcm[:MAX_AUDIO_BYTES]


def valid_turn_id(turn_id: str) -> bool:
    return bool(_PATH_ID.fullmatch(turn_id))


class StoredSpeech:
    mode = "stored"

    def __init__(self, tts: TextToSpeech, store: AudioStore) -> None:
        self.tts, self.store = tts, store

    def deliver(self, device_id: str, text: str, lang: str) -> dict | None:
        speech = self.tts.synthesize(text, lang)  # ProviderError -> the caller adds a warning
        pcm = _bounded(speech.pcm)
        if not pcm:
            return None
        turn_id = self.store.put(device_id, pcm, speech.sample_rate)
        return {"path": f"/v1/turns/{turn_id}/audio", "format": "pcm_s16le", "sample_rate": speech.sample_rate,
                "bytes": len(pcm), "duration_ms": int(len(pcm) / 2 / speech.sample_rate * 1000)}

    def fetch(self, turn_id: str, device_id: str) -> tuple[bytes, int] | None:
        item = self.store.take(turn_id, device_id)
        return None if item is None else (item.pcm, item.rate)


class TicketSpeech:
    mode = "ticket"

    def __init__(self, tts: TextToSpeech, sealer: Sealer, ttl_seconds: int) -> None:
        self.tts, self.sealer, self.ttl = tts, sealer, ttl_seconds

    def _rate(self) -> int:
        rate = getattr(self.tts, "rate", 0)
        if rate not in (8000, 16000, 22050, 24000):
            raise ProviderError("tts_rate_unknown", retryable=False)
        return rate

    def deliver(self, device_id: str, text: str, lang: str, now: float | None = None) -> dict | None:
        rate = self._rate()
        exp = int((time.time() if now is None else now) + self.ttl)
        cut = text
        while len(cut) >= 12:
            ticket = self.sealer.seal({"x": cut, "l": lang, "r": rate, "e": exp}, device_id)
            if len(ticket) <= TICKET_MAX_CHARS:
                # Duration and size are known only after synthesis on the GET.
                return {"path": f"/v1/turns/{ticket}/audio", "format": "pcm_s16le", "sample_rate": rate}
            cut = cut[: int(len(cut) * 0.8)].rsplit(" ", 1)[0]  # very long reply: speak its beginning
        return None

    def fetch(self, turn_id: str, device_id: str,
              now: float | None = None) -> tuple[bytes | Iterator[bytes], int] | None:
        t = self.sealer.open(turn_id, device_id, TICKET_MAX_CHARS)
        now = time.time() if now is None else now
        if t is None or not isinstance(t.get("e"), int) or t["e"] < now or not isinstance(t.get("x"), str):
            return None
        lang = t.get("l") if t.get("l") in ("en", "ar") else "en"
        chunked = getattr(self.tts, "synthesize_chunks", None)
        if chunked is None:
            speech = self.tts.synthesize(t["x"], lang)
            if speech.sample_rate != t.get("r"):
                raise ProviderError("tts_rate_mismatch", retryable=False)
            return _bounded(speech.pcm), speech.sample_rate
        pieces = iter(chunked(t["x"], lang))
        first = next(pieces, None)  # ProviderError here becomes the HTTP status of the GET
        if first is None:
            return b"", t["r"]
        if first.sample_rate != t.get("r"):
            raise ProviderError("tts_rate_mismatch", retryable=False)
        return _stream(itertools.chain([first], pieces)), first.sample_rate


def _stream(pieces) -> Iterator[bytes]:
    """Yield PCM piece by piece, never more than MAX_AUDIO_BYTES in total."""
    sent = 0
    try:
        for sp in pieces:
            pcm = sp.pcm[: max(0, MAX_AUDIO_BYTES - sent)]
            pcm = pcm[: len(pcm) - len(pcm) % 2]
            if not pcm:
                break
            sent += len(pcm)
            yield pcm
    except ProviderError as exc:  # a later piece failed: the robot plays what it already has
        log.warning("speech stream ended early: %s", exc.code)
