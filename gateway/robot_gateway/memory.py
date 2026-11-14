"""Conversation memory that survives serverless hosting.

On Vercel (or any host that runs several short-lived instances) the next turn of a
conversation may reach a different instance, and nothing in process memory or on disk can be
relied on. So the gateway returns the conversation context with every reply:

    reply JSON  : "ctx": "<sealed token>"           (the robot keeps it in RAM)
    next request: X-Robot-Context: <sealed token>   (the robot sends it back)

The token holds the tutor state (learning mode, pending question, level, quiz score) and the
last few exchanges for the language model. It is sealed (see sealing.py): encrypted, bound
to the device, and it expires after SESSION_TTL_SECONDS. A missing, expired, foreign or
edited token starts a fresh conversation; it can never inject words into the model's history.
Its size is capped (CONTEXT_MAX_CHARS) by dropping the oldest exchanges first.

On a long-running host the gateway also keeps sessions in memory (SessionStore), so robots
with older firmware that do not send the token back keep their memory there.
"""
from __future__ import annotations

import time
from fractions import Fraction

from .providers.base import ChatMessage
from .sealing import Sealer
from .state import SessionStore
from .tutor.engine import Content, Pending, Session

MODES = {"chat", "alphabet", "counting", "math", "vocab_en", "vocab_ar", "quiz", "story", "wisdom"}
KINDS = {"letter", "number", "math", "word"}
TOKEN_HARD_LIMIT = 4096  # never accept more than this from a request header


def _int(v, lo: int, hi: int) -> int:
    if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
        raise ValueError("int out of range")
    return v


def _str(v, max_len: int) -> str:
    if not isinstance(v, str) or len(v) > max_len:
        raise ValueError("bad string")
    return v


def _content_to(c: Content | None):
    return None if c is None else [c.kind, c.value, c.caption]


def _content_from(v) -> Content | None:
    if v is None:
        return None
    if not isinstance(v, list) or len(v) != 3 or v[0] not in KINDS:
        raise ValueError("bad content")
    return Content(v[0], _str(v[1], 64), _str(v[2], 160))


def session_to_state(s: Session) -> dict:
    p = s.pending
    pending = None if p is None else {
        "n": None if p.expected_number is None else str(p.expected_number),
        "a": sorted(p.accepted),
        "t": p.answer_text,
        "at": p.attempts,
        "ng": p.nudged,
        "c": _content_to(p.content_answer),
    }
    return {"m": s.mode, "i": s.index, "lv": s.level, "st": s.streak, "mi": s.misses,
            "qa": s.quiz_asked, "qc": s.quiz_correct, "p": pending}


def session_from_state(device_id: str, st: dict, history: list) -> Session:
    if st.get("m") not in MODES:
        raise ValueError("bad mode")
    s = Session(device_id, mode=st["m"], index=_int(st.get("i"), 0, 10**6), level=_int(st.get("lv"), 0, 3),
                streak=_int(st.get("st"), 0, 100), misses=_int(st.get("mi"), 0, 100),
                quiz_asked=_int(st.get("qa"), 0, 100), quiz_correct=_int(st.get("qc"), 0, 100))
    p = st.get("p")
    if p is not None:
        if not isinstance(p, dict):
            raise ValueError("bad pending")
        n = p.get("n")
        accepted = p.get("a")
        if not isinstance(accepted, list) or len(accepted) > 32:
            raise ValueError("bad accepted")
        s.pending = Pending(
            expected_number=None if n is None else Fraction(_str(n, 32)),
            accepted={_str(a, 80) for a in accepted},
            answer_text=_str(p.get("t"), 80),
            attempts=_int(p.get("at"), 0, 100),
            nudged=bool(p.get("ng")),
            content_answer=_content_from(p.get("c")),
        )
    if not isinstance(history, list):
        raise ValueError("bad history")
    for item in history[-16:]:
        if not isinstance(item, list) or len(item) != 2 or item[0] not in ("u", "a"):
            raise ValueError("bad history item")
        s.history.append(ChatMessage("user" if item[0] == "u" else "assistant", _str(item[1], 2000)))
    return s


class ContextCodec:
    VERSION = 1

    def __init__(self, sealer: Sealer, ttl_seconds: int, max_chars: int = 2400, history_turns: int = 4,
                 message_chars: int = 400) -> None:
        self.sealer, self.ttl = sealer, ttl_seconds
        self.max_chars = max(256, min(max_chars, TOKEN_HARD_LIMIT))
        self.history_turns, self.message_chars = history_turns, message_chars

    def encode(self, s: Session, now: float | None = None) -> str:
        """Seal the session; drops the oldest exchanges until the token fits max_chars."""
        iat = int(time.time() if now is None else now)
        msgs = list(s.history)[-2 * self.history_turns:] if self.history_turns > 0 else []
        hist = [["u" if m.role == "user" else "a", m.text[: self.message_chars]] for m in msgs]
        if hist and hist[0][0] == "a":
            hist = hist[1:]  # keep user/assistant pairs, starting with the child
        state = session_to_state(s)
        while True:
            token = self.sealer.seal({"v": self.VERSION, "iat": iat, "s": state, "h": hist}, s.device_id)
            if len(token) <= self.max_chars:
                return token
            if not hist:
                return ""  # cannot happen with the default limits; the robot then clears its copy
            hist = hist[2:]

    def decode(self, token: str | None, device_id: str, now: float | None = None) -> Session | None:
        obj = self.sealer.open(token, device_id, TOKEN_HARD_LIMIT)
        if obj is None or obj.get("v") != self.VERSION:
            return None
        now = time.time() if now is None else now
        iat = obj.get("iat")
        if not isinstance(iat, int) or not (-60 <= now - iat <= self.ttl):
            return None  # expired (or from the future)
        try:
            st = obj.get("s")
            if not isinstance(st, dict):
                return None
            return session_from_state(device_id, st, obj.get("h", []))
        except (ValueError, TypeError, ZeroDivisionError):
            return None


class ConversationMemory:
    """Where a turn's session comes from: the robot's token first, then (if enabled) server memory."""

    def __init__(self, codec: ContextCodec, store: SessionStore | None) -> None:
        self.codec, self.store = codec, store

    @property
    def mode(self) -> str:
        return "context" if self.store is None else "context+server"

    def load(self, device_id: str, token: str | None) -> tuple[Session, str]:
        if token:
            s = self.codec.decode(token, device_id)
            if s is not None:
                if self.store is not None:
                    self.store.put(s)
                return s, "ctx"
        if self.store is not None:
            return self.store.get(device_id), ("server" if not token else "server_ctx_rejected")
        return Session(device_id), ("ctx_rejected" if token else "new")

    def token_for(self, s: Session) -> str:
        return self.codec.encode(s)


__all__ = ["ContextCodec", "ConversationMemory", "session_to_state", "session_from_state"]
