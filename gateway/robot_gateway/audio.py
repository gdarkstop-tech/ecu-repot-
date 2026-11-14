"""Small audio helpers for incoming PCM (signed 16-bit little-endian mono)."""
from __future__ import annotations

import array
import math


def voiced_seconds(pcm: bytes, sample_rate: int, rms_threshold: int) -> float:
    """Seconds of 20 ms frames whose RMS exceeds `rms_threshold`.

    Used to avoid paying for speech recognition on silence or a click: the robot's VAD already
    filters most of these, this is a cheap second check on the server.
    """
    samples = array.array("h")
    samples.frombytes(pcm[: len(pcm) - (len(pcm) % 2)])
    frame = max(1, sample_rate // 50)
    voiced = 0
    for i in range(0, len(samples) - frame + 1, frame):
        chunk = samples[i : i + frame]
        if math.sqrt(sum(s * s for s in chunk) / frame) > rms_threshold:
            voiced += 1
    return voiced * frame / sample_rate
