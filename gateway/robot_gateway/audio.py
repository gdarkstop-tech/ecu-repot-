"""Small audio helpers: incoming PCM checks and outgoing speech conversion (s16le mono)."""
from __future__ import annotations

import array
import math
import sys


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


# ---------------------------------------------------------------- outgoing speech
# The robot plays signed 16-bit little-endian mono PCM at one of these rates (firmware contract).
ROBOT_RATES = (8000, 16000, 22050, 24000)
PEAK_LIMIT = 29_204  # -1 dBFS: never hand the amplifier a clipped waveform


def decode_wav(data: bytes, max_bytes: int = 16_000_000) -> tuple[bytes, int]:
    """WAV (PCM 8/16/24/32-bit or 32-bit float, any channel count) -> (s16le mono PCM, rate).

    Tolerates streaming-style headers whose data size is 0 or 0xFFFFFFFF (the rest of the file is
    the data). Raises ValueError for anything that is not a playable WAV.
    """
    if len(data) > max_bytes or len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    fmt = None
    pos = 12
    payload = b""
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], int.from_bytes(data[pos + 4:pos + 8], "little")
        body = pos + 8
        if cid == b"fmt ":
            if size < 16:
                raise ValueError("short fmt chunk")
            tag = int.from_bytes(data[body:body + 2], "little")
            channels = int.from_bytes(data[body + 2:body + 4], "little")
            rate = int.from_bytes(data[body + 4:body + 8], "little")
            bits = int.from_bytes(data[body + 14:body + 16], "little")
            if tag == 0xFFFE and size >= 40:  # WAVE_FORMAT_EXTENSIBLE: the real tag is in the GUID
                tag = int.from_bytes(data[body + 24:body + 26], "little")
            fmt = (tag, channels, rate, bits)
        elif cid == b"data":
            end = len(data) if size in (0, 0xFFFFFFFF) or body + size > len(data) else body + size
            payload = data[body:end]
            break
        pos = body + size + (size & 1)
    if fmt is None:
        raise ValueError("no fmt chunk")
    tag, channels, rate, bits = fmt
    if channels < 1 or channels > 8 or not 4000 <= rate <= 96000:
        raise ValueError("unsupported WAV layout")
    width = bits // 8
    frame = width * channels
    payload = payload[: len(payload) - len(payload) % frame] if frame else b""
    if tag == 1 and bits == 16:
        samples = array.array("h")
        samples.frombytes(payload)
        if sys.byteorder != "little":
            samples.byteswap()
        values = samples
    elif tag == 1 and bits == 8:
        values = array.array("h", ((b - 128) << 8 for b in payload))
    elif tag == 1 and bits in (24, 32):
        values = array.array("h", (int.from_bytes(payload[i + width - 2:i + width], "little", signed=True)
                                   for i in range(0, len(payload), width)))
    elif tag == 3 and bits == 32:
        floats = array.array("f")
        floats.frombytes(payload)
        if sys.byteorder != "little":
            floats.byteswap()
        values = array.array("h", (max(-32768, min(32767, int(f * 32767))) for f in floats))
    else:
        raise ValueError(f"unsupported WAV encoding (format {tag}, {bits} bit)")
    if channels > 1:
        values = array.array("h", (sum(values[i:i + channels]) // channels
                                   for i in range(0, len(values), channels)))
    return _le_bytes(values), rate


def resample(pcm: bytes, rate: int, target: int) -> bytes:
    """Linear-interpolation resampler for s16le mono (speech quality; used only on a rate mismatch)."""
    if rate == target or not pcm:
        return pcm
    src = array.array("h")
    src.frombytes(pcm[: len(pcm) - len(pcm) % 2])
    if sys.byteorder != "little":
        src.byteswap()
    n_out = int(len(src) * target / rate)
    step = rate / target
    last = len(src) - 1
    out = array.array("h", bytes(2 * n_out))
    for i in range(n_out):
        x = i * step
        j = int(x)
        if j >= last:
            out[i] = src[last]
        else:
            f = x - j
            out[i] = int(src[j] + (src[j + 1] - src[j]) * f)
    return _le_bytes(out)


def limit_peak(pcm: bytes, peak: int = PEAK_LIMIT) -> bytes:
    """Scale down (never up) so no sample exceeds `peak`."""
    samples = array.array("h")
    samples.frombytes(pcm[: len(pcm) - len(pcm) % 2])
    if sys.byteorder != "little":
        samples.byteswap()
    top = max((abs(s) for s in samples), default=0)
    if top <= peak:
        return pcm[: len(samples) * 2]
    k = peak / top
    return _le_bytes(array.array("h", (int(s * k) for s in samples)))


def silence(seconds: float, rate: int) -> bytes:
    return bytes(2 * int(seconds * rate))


def _le_bytes(samples: array.array) -> bytes:
    if sys.byteorder != "little":
        samples = array.array("h", samples)
        samples.byteswap()
    return samples.tobytes()
