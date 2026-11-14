"""In-process runtime state: rate limits, tutor sessions, and one-time reply audio.

Used on long-running hosts (Docker, a VPS: one worker). On serverless hosts (Vercel) the
gateway runs stateless instead: conversation memory travels with the robot as a sealed token
(memory.py) and reply audio is described by a sealed ticket (speech.py). The rate limiter
then only protects each instance (best effort); see docs/ARCHITECTURE.md (ADR-11).
Privacy: reply audio is held only until the robot fetches it (or the TTL expires); the
robot's microphone audio is never stored.
"""
from __future__ import annotations

import secrets
import threading
import time
from collections import deque
from dataclasses import dataclass

from .tutor.engine import Session


class RateLimiter:
    def __init__(self, per_minute: int, per_day: int) -> None:
        self.per_minute, self.per_day = per_minute, per_day
        self._minute: dict[str, deque] = {}
        self._day: dict[str, tuple[int, int]] = {}
        self._lock = threading.Lock()

    def allow(self, key: str, now: float | None = None) -> bool:
        now = time.time() if now is None else now
        day = int(now // 86400)
        with self._lock:
            q = self._minute.setdefault(key, deque())
            while q and now - q[0] >= 60:
                q.popleft()
            d, n = self._day.get(key, (day, 0))
            if d != day:
                d, n = day, 0
            if len(q) >= self.per_minute or n >= self.per_day:
                return False
            q.append(now)
            self._day[key] = (d, n + 1)
            return True


class SessionStore:
    def __init__(self, ttl_seconds: int) -> None:
        self.ttl = ttl_seconds
        self._s: dict[str, Session] = {}
        self._lock = threading.Lock()

    def get(self, device_id: str) -> Session:
        now = time.monotonic()
        with self._lock:
            for k in [k for k, v in self._s.items() if now - v.last_seen > self.ttl]:
                del self._s[k]  # forget idle conversations
            s = self._s.get(device_id)
            if s is None:
                s = self._s[device_id] = Session(device_id)
            s.last_seen = now
            return s

    def put(self, session: Session) -> None:
        """Replace a device's session (restored from the robot's context token)."""
        session.last_seen = time.monotonic()
        with self._lock:
            self._s[session.device_id] = session


@dataclass
class StoredAudio:
    device_id: str
    pcm: bytes
    rate: int
    expires: float


class AudioStore:
    def __init__(self, ttl_seconds: int) -> None:
        self.ttl = ttl_seconds
        self._a: dict[str, StoredAudio] = {}
        self._lock = threading.Lock()

    def put(self, device_id: str, pcm: bytes, rate: int) -> str:
        turn_id = secrets.token_urlsafe(16)
        now = time.monotonic()
        with self._lock:
            for k in [k for k, v in self._a.items() if v.expires < now]:
                del self._a[k]
            self._a[turn_id] = StoredAudio(device_id, pcm, rate, now + self.ttl)
        return turn_id

    def take(self, turn_id: str, device_id: str) -> StoredAudio | None:
        """One-time fetch, only by the device that made the turn."""
        with self._lock:
            item = self._a.get(turn_id)
            if not item or item.device_id != device_id or item.expires < time.monotonic():
                return None
            del self._a[turn_id]
            return item
