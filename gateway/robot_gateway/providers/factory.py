"""Builds the configured providers. Switching provider is a configuration change only."""
from __future__ import annotations

from ..config import Settings
from .base import LanguageModel, ProviderError, SpeechToText, TextToSpeech
from .mock import MockLLM, MockSTT, MockTTS


def build_stt(s: Settings) -> SpeechToText:
    if s.stt_provider == "mock":
        return MockSTT()
    if s.stt_provider == "openai":
        from .openai_compat import OpenAISTT
        return OpenAISTT(s.openai_base_url, s.openai_stt_model)
    raise ProviderError(f"unknown STT_PROVIDER {s.stt_provider!r}", retryable=False)


def build_llm(s: Settings) -> LanguageModel:
    if s.llm_provider == "mock":
        return MockLLM()
    if s.llm_provider == "anthropic":
        from .anthropic_llm import AnthropicLLM
        return AnthropicLLM(s.anthropic_model, s.anthropic_effort, s.anthropic_fallbacks)
    if s.llm_provider == "openai":
        from .openai_compat import OpenAILLM
        return OpenAILLM(s.openai_base_url, s.openai_llm_model)
    raise ProviderError(f"unknown LLM_PROVIDER {s.llm_provider!r}", retryable=False)


def build_tts(s: Settings) -> TextToSpeech:
    if s.tts_provider == "mock":
        return MockTTS()
    if s.tts_provider == "espeak":
        from .espeak import EspeakTTS
        return EspeakTTS()
    if s.tts_provider == "openai":
        from .openai_compat import OpenAITTS
        return OpenAITTS(s.openai_base_url, s.openai_tts_model, s.openai_tts_voice)
    raise ProviderError(f"unknown TTS_PROVIDER {s.tts_provider!r}", retryable=False)
