"""Sealed tokens: small encrypted + authenticated blobs the gateway hands out and later reads back.

Used where a serverless host cannot keep anything between requests (no process memory, no
files): the conversation context the robot carries (`ctx`) and the reply-audio ticket in the
audio URL. AES-256-GCM gives confidentiality and integrity; the associated data binds every
token to its purpose and to one device, so a token can neither be forged, edited, read,
nor replayed by another robot.

Key: CONTEXT_SECRET (>= 32 characters, set it on the host). Without it the key is derived
from the provider API keys (stable across instances, changes when a key is rotated), and as a
last resort a random per-process key is used (memory then works within one instance only).
/healthz reports which source is in use — never the key.
"""
from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import re
import zlib
from typing import Mapping

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

_B64URL = re.compile(r"[A-Za-z0-9_-]+")
_NONCE = 12
_TAG = 16
_MAX_PLAIN = 64 * 1024  # decompression bound (tokens are small; this blocks zip bombs)

PROVIDER_KEY_VARS = ("ANTHROPIC_API_KEY", "GROQ_API_KEY", "OPENAI_API_KEY", "STT_API_KEY")


def resolve_context_secret(environ: Mapping[str, str] | None = None) -> tuple[bytes, str, str | None]:
    """Return (secret, source, warning). source: CONTEXT_SECRET | derived | random."""
    env = os.environ if environ is None else environ
    configured = (env.get("CONTEXT_SECRET") or "").strip()
    if len(configured) >= 32:
        return configured.encode("utf-8"), "CONTEXT_SECRET", None
    why = "CONTEXT_SECRET is shorter than 32 characters" if configured else "CONTEXT_SECRET is not set"
    keys = [env[k] for k in PROVIDER_KEY_VARS if env.get(k)]
    if keys:
        material = "\x00".join(keys).encode("utf-8")
        return (hashlib.sha256(b"robot-gateway context key\x00" + material).digest(), "derived",
                why + ": using a key derived from the provider API keys")
    return os.urandom(32), "random", why + " and no provider key exists: memory works within one instance only"


class Sealer:
    def __init__(self, secret: bytes, purpose: str) -> None:
        key = hmac.new(secret, b"robot-gateway/" + purpose.encode("ascii"), hashlib.sha256).digest()
        self._aead = AESGCM(key)
        self._purpose = purpose.encode("ascii")

    def _aad(self, bound_to: str) -> bytes:
        return self._purpose + b"\x00" + bound_to.encode("utf-8")

    def seal(self, obj: dict, bound_to: str) -> str:
        raw = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        nonce = os.urandom(_NONCE)
        blob = nonce + self._aead.encrypt(nonce, zlib.compress(raw, 9), self._aad(bound_to))
        return base64.urlsafe_b64encode(blob).rstrip(b"=").decode("ascii")

    def open(self, token: str | None, bound_to: str, max_chars: int) -> dict | None:
        """The decoded object, or None for anything missing, malformed, foreign or tampered with."""
        if not token or len(token) > max_chars or not _B64URL.fullmatch(token):
            return None
        try:
            blob = base64.urlsafe_b64decode(token + "=" * (-len(token) % 4))
            if len(blob) < _NONCE + _TAG:
                return None
            packed = self._aead.decrypt(blob[:_NONCE], blob[_NONCE:], self._aad(bound_to))
            inflater = zlib.decompressobj()
            raw = inflater.decompress(packed, _MAX_PLAIN)
            if inflater.unconsumed_tail or not inflater.eof:
                return None
            obj = json.loads(raw.decode("utf-8"))
        except (ValueError, InvalidTag, zlib.error):
            return None
        return obj if isinstance(obj, dict) else None
