"""Offline text-to-speech with eSpeak NG (English and Arabic, robotic voice, no API key).

Install on the gateway host:  apt-get install espeak-ng   (the Dockerfile does this).
"""
from __future__ import annotations

import io
import shutil
import subprocess
import wave

from .base import ProviderError, Speech

VOICES = {"en": "en-us", "ar": "ar"}


class EspeakTTS:
    name = "espeak"
    rate = 22050  # eSpeak NG's output rate

    def __init__(self) -> None:
        self.binary = shutil.which("espeak-ng")
        if not self.binary:
            raise ProviderError("espeak_not_installed", retryable=False)

    def synthesize(self, text: str, language: str) -> Speech:
        voice = VOICES.get(language, "en-us")
        try:
            # Text goes via stdin (never the command line) so it cannot be read as options.
            proc = subprocess.run(
                [self.binary, "-v", voice, "-s", "150", "--stdin", "--stdout"],
                input=text.encode("utf-8"), capture_output=True, timeout=20, check=True,
            )
        except (subprocess.SubprocessError, OSError) as exc:
            raise ProviderError("tts_failed") from exc
        with wave.open(io.BytesIO(proc.stdout)) as w:
            if w.getsampwidth() != 2 or w.getnchannels() != 1:
                raise ProviderError("tts_bad_format")
            return Speech(w.readframes(w.getnframes()), w.getframerate())
