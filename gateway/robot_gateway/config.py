"""Gateway settings, read from environment variables (and an optional local .env file).

Provider API keys (ANTHROPIC_API_KEY, OPENAI_API_KEY) are read by the providers directly
from the environment and are never logged, echoed, or returned by any endpoint.
"""
from __future__ import annotations

import os
import shutil
from dataclasses import dataclass, field
from pathlib import Path


def load_dotenv(path: str | os.PathLike = ".env") -> None:
    """Minimal .env loader for local development. Existing variables win."""
    p = Path(path)
    if not p.is_file():
        return
    for raw in p.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key, value = key.strip(), value.strip().strip('"').strip("'")
        os.environ.setdefault(key, value)


def _bool(name: str, default: bool) -> bool:
    v = os.environ.get(name)
    return default if v is None else v.strip().lower() in {"1", "true", "yes", "on"}


def _int(name: str, default: int) -> int:
    v = os.environ.get(name)
    return default if not v else int(v)


def _float(name: str, default: float) -> float:
    v = os.environ.get(name)
    return default if not v else float(v)


@dataclass(frozen=True)
class Settings:
    # Providers: each one is swappable without touching the robot firmware.
    stt_provider: str = "mock"      # mock | openai
    llm_provider: str = "mock"      # mock | anthropic | openai
    tts_provider: str = "mock"      # mock | espeak | openai

    anthropic_model: str = "claude-opus-5-5"
    anthropic_effort: str = "low"   # short spoken replies: low effort keeps latency down
    anthropic_fallbacks: bool = True

    openai_base_url: str = "https://api.openai.com/v1"
    openai_llm_model: str = ""
    openai_stt_model: str = ""
    openai_tts_model: str = ""
    openai_tts_voice: str = "alloy"

    robot_name: str = "Robo"
    devices_file: str = "devices.json"
    device_token_hashes: str = ""   # alternative to the file: "robot-1:<sha256>,robot-2:<sha256>"

    max_audio_seconds: float = 15.0
    max_text_chars: int = 500
    turns_per_minute: int = 20
    turns_per_day: int = 1500
    audio_ttl_seconds: int = 120
    session_ttl_seconds: int = 1800
    history_turns: int = 8

    log_transcripts: bool = False   # children's speech is not logged unless you opt in
    allow_debug_transcript: bool = False  # X-Debug-Transcript header (test environments only)
    extra: dict = field(default_factory=dict)

    @classmethod
    def from_env(cls) -> "Settings":
        default_tts = "espeak" if shutil.which("espeak-ng") else "mock"
        return cls(
            stt_provider=os.environ.get("STT_PROVIDER", "mock").lower(),
            llm_provider=os.environ.get("LLM_PROVIDER", "mock").lower(),
            tts_provider=os.environ.get("TTS_PROVIDER", default_tts).lower(),
            anthropic_model=os.environ.get("ANTHROPIC_MODEL", "claude-opus-5-5"),
            anthropic_effort=os.environ.get("ANTHROPIC_EFFORT", "low"),
            anthropic_fallbacks=_bool("ANTHROPIC_FALLBACKS", True),
            openai_base_url=os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1").rstrip("/"),
            openai_llm_model=os.environ.get("OPENAI_LLM_MODEL", ""),
            openai_stt_model=os.environ.get("OPENAI_STT_MODEL", ""),
            openai_tts_model=os.environ.get("OPENAI_TTS_MODEL", ""),
            openai_tts_voice=os.environ.get("OPENAI_TTS_VOICE", "alloy"),
            robot_name=os.environ.get("ROBOT_NAME", "Robo"),
            devices_file=os.environ.get("DEVICES_FILE", "devices.json"),
            device_token_hashes=os.environ.get("DEVICE_TOKEN_HASHES", ""),
            max_audio_seconds=_float("MAX_AUDIO_SECONDS", 15.0),
            max_text_chars=_int("MAX_TEXT_CHARS", 500),
            turns_per_minute=_int("TURNS_PER_MINUTE", 20),
            turns_per_day=_int("TURNS_PER_DAY", 1500),
            audio_ttl_seconds=_int("AUDIO_TTL_SECONDS", 120),
            session_ttl_seconds=_int("SESSION_TTL_SECONDS", 1800),
            history_turns=_int("HISTORY_TURNS", 8),
            log_transcripts=_bool("LOG_TRANSCRIPTS", False),
            allow_debug_transcript=_bool("ALLOW_DEBUG_TRANSCRIPT", False),
        )
