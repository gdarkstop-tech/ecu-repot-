"""Gateway settings, read from environment variables (and an optional local .env file).

Provider API keys (GROQ_API_KEY, ANTHROPIC_API_KEY, OPENAI_API_KEY) are read by the providers
directly from the environment and are never logged, echoed, or returned by any endpoint.

Default providers: when LLM_PROVIDER / STT_PROVIDER are not set, Groq is used if GROQ_API_KEY
exists (free plan available), otherwise the keyless demo (mock). Text-to-speech stays opt-in
(TTS_PROVIDER=groq for the natural Orpheus voice) because Groq's free speech quota is small.
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


def _stateless() -> bool:
    """GATEWAY_STATELESS=1/0 forces the mode; by default it is on when running on Vercel."""
    v = (os.environ.get("GATEWAY_STATELESS") or "auto").strip().lower()
    if v in {"1", "true", "yes", "on"}:
        return True
    if v in {"0", "false", "no", "off"}:
        return False
    return bool(os.environ.get("VERCEL"))


def _float(name: str, default: float) -> float:
    v = os.environ.get(name)
    return default if not v else float(v)


@dataclass(frozen=True)
class Settings:
    # Providers: each one is swappable without touching the robot firmware.
    stt_provider: str = "mock"      # mock | groq | openai | openai_compatible
    llm_provider: str = "mock"      # mock | groq | anthropic | openai
    tts_provider: str = "mock"      # mock | groq | espeak | openai

    # Groq (one GROQ_API_KEY for speech recognition, the language model and the voice).
    groq_llm_model: str = "openai/gpt-oss-120b"
    groq_reasoning_effort: str = "low"   # GPT-OSS: low = fastest, fewest tokens (free-plan friendly)
    llm_max_tokens: int = 1024           # reasoning + answer, for OpenAI-compatible chat models
    llm_temperature: float = 0.6
    groq_tts_model_en: str = "canopylabs/orpheus-v1-english"
    groq_tts_model_ar: str = "canopylabs/orpheus-arabic-saudi"
    groq_tts_voice_en: str = "hannah"
    groq_tts_voice_ar: str = "lulwa"
    groq_tts_max_chunks: int = 2         # pieces of <= 200 characters spoken per answer

    # Claude Haiku 5.5: the fast, low-cost Claude model, chosen for short conversational replies.
    anthropic_model: str = "claude-haiku-5-5"
    anthropic_effort: str = "low"   # low effort = fastest, cheapest; raise for harder tutoring
    anthropic_fallbacks: bool = True  # only sent to models that support server-side fallbacks
    anthropic_max_tokens: int = 2048

    stt_base_url: str = ""          # override the preset's endpoint
    stt_model: str = ""             # override the preset's model
    stt_response_format: str = ""   # "verbose_json" (Whisper: language + no-speech scores) or "json"
    min_voiced_seconds: float = 0.25  # below this the audio is not sent to the paid STT at all
    voice_rms: int = 150              # 16-bit RMS that counts as "voiced" (~ -47 dBFS)

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

    # Serverless hosting (Vercel): keep nothing in process memory between requests. Conversation
    # memory travels with the robot as a sealed token (memory.py); reply audio uses sealed tickets.
    stateless: bool = False
    context_max_chars: int = 2400   # size cap of the robot's context token (its buffer is 3000)
    context_history_turns: int = 4  # exchanges carried in the token for the language model

    log_transcripts: bool = False   # children's speech is not logged unless you opt in
    allow_debug_transcript: bool = False  # X-Debug-Transcript header (test environments only)
    extra: dict = field(default_factory=dict)

    @classmethod
    def from_env(cls) -> "Settings":
        default_tts = "espeak" if shutil.which("espeak-ng") else "mock"
        default_ai = "groq" if os.environ.get("GROQ_API_KEY") else "mock"
        return cls(
            stt_provider=(os.environ.get("STT_PROVIDER") or default_ai).strip().lower(),
            llm_provider=(os.environ.get("LLM_PROVIDER") or default_ai).strip().lower(),
            tts_provider=(os.environ.get("TTS_PROVIDER") or default_tts).strip().lower(),
            groq_llm_model=os.environ.get("GROQ_LLM_MODEL") or "openai/gpt-oss-120b",
            groq_reasoning_effort=os.environ.get("GROQ_REASONING_EFFORT", "low").strip().lower(),
            llm_max_tokens=_int("LLM_MAX_TOKENS", 1024),
            llm_temperature=_float("LLM_TEMPERATURE", 0.6),
            groq_tts_model_en=os.environ.get("GROQ_TTS_MODEL_EN") or "canopylabs/orpheus-v1-english",
            groq_tts_model_ar=os.environ.get("GROQ_TTS_MODEL_AR") or "canopylabs/orpheus-arabic-saudi",
            groq_tts_voice_en=os.environ.get("GROQ_TTS_VOICE_EN") or "hannah",
            groq_tts_voice_ar=os.environ.get("GROQ_TTS_VOICE_AR") or "lulwa",
            groq_tts_max_chunks=_int("GROQ_TTS_MAX_CHUNKS", 2),
            anthropic_model=os.environ.get("ANTHROPIC_MODEL", "claude-haiku-5-5"),
            anthropic_effort=os.environ.get("ANTHROPIC_EFFORT", "low"),
            anthropic_fallbacks=_bool("ANTHROPIC_FALLBACKS", True),
            anthropic_max_tokens=_int("ANTHROPIC_MAX_TOKENS", 2048),
            stt_base_url=os.environ.get("STT_BASE_URL", ""),
            stt_model=os.environ.get("STT_MODEL", ""),
            stt_response_format=os.environ.get("STT_RESPONSE_FORMAT", ""),
            min_voiced_seconds=_float("MIN_VOICED_SECONDS", 0.25),
            voice_rms=_int("VOICE_RMS", 150),
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
            stateless=_stateless(),
            context_max_chars=_int("CONTEXT_MAX_CHARS", 2400),
            context_history_turns=_int("CONTEXT_HISTORY_TURNS", 4),
            log_transcripts=_bool("LOG_TRANSCRIPTS", False),
            allow_debug_transcript=_bool("ALLOW_DEBUG_TRANSCRIPT", False),
        )
