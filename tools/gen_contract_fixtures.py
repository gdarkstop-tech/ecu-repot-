"""Capture REAL gateway replies as fixtures for the firmware's parser tests.

The firmware test (firmware/test/test_gateway_contract) parses these files with the same C++
code the robot runs, so a change to the gateway's JSON that would break the robot fails CI.

    python tools/gen_contract_fixtures.py
"""
from __future__ import annotations

import json
import math
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "gateway"))

from fastapi.testclient import TestClient  # noqa: E402

from robot_gateway.app import create_app  # noqa: E402
from robot_gateway.config import Settings  # noqa: E402
from robot_gateway.devices import DeviceRegistry, hash_token  # noqa: E402
from robot_gateway.providers.mock import MockLLM, MockSTT, MockTTS  # noqa: E402

OUT = ROOT / "firmware/test/test_gateway_contract/fixtures"
TOKEN = "rbt_fixture_token_0123456789"


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    reg = DeviceRegistry(None, f"robot-1:{hash_token(TOKEN)}")
    c = TestClient(create_app(Settings(), stt=MockSTT(), llm=MockLLM(), tts=MockTTS(), registry=reg,
                              stateless=False, context_secret=b"fixture-secret-0123456789abcdefghij"))
    h = {"Authorization": f"Bearer {TOKEN}"}
    cases = {
        "math_en": "what is 7 plus 5?",
        "math_ar": "كم ستة ضرب سبعة؟",
        "alphabet_en": "let's learn the alphabet",
        "alphabet_ar": "علمني الحروف",
        "story_ar": "احك لي قصة",
        "vocab_ar": "teach me arabic words",
    }
    for name, text in cases.items():
        r = c.post("/v1/turns/text", headers=h, json={"text": text})
        (OUT / f"{name}.json").write_text(r.text, encoding="utf-8")
    text_mode = {**h, "X-Robot-Output": "text"}
    for name, text in {"textmode_story_en": "tell me a story", "textmode_story_ar": "احك لي قصة",
                       "textmode_math_ar": "كم سبعة زائد خمسة؟"}.items():
        r = c.post("/v1/turns/text", headers=text_mode, json={"text": text})
        (OUT / f"{name}.json").write_text(r.text, encoding="utf-8")
    silence = b"\x00\x00" * 16000
    r = c.post("/v1/turns", headers={**h, "Content-Type": "audio/L16;rate=16000;channels=1"}, content=silence)
    (OUT / "ignore.json").write_text(r.text, encoding="utf-8")
    tone = b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 300 * i / 16000))) for i in range(24000))
    r = c.post("/v1/turns", headers={**h, "Content-Type": "audio/L16;rate=16000;channels=1"}, content=tone)
    (OUT / "demo_audio.json").write_text(r.text, encoding="utf-8")

    # Serverless mode (Vercel): conversation context token + sealed audio tickets.
    v = TestClient(create_app(Settings(), stt=MockSTT(), llm=MockLLM(), tts=MockTTS(), registry=reg,
                              stateless=True, context_secret=b"fixture-secret-0123456789abcdefghij"))
    r1 = v.post("/v1/turns/text", headers=text_mode, json={"text": "let's do math"})
    (OUT / "vercel_math_turn1.json").write_text(r1.text, encoding="utf-8")
    q = r1.json()["display"]["content"]["value"]  # e.g. "3 + 4 = ?"
    a, op, b = q.split()[:3]
    answer = int(a) + int(b) if op == "+" else int(a) - int(b) if op == "-" else int(a) * int(b)
    r2 = v.post("/v1/turns/text", headers={**text_mode, "X-Robot-Context": r1.json()["ctx"]},
                json={"text": str(answer)})
    assert r2.json()["reply_text"].startswith(("Correct", "Yes", "That's right")), r2.text
    (OUT / "vercel_math_turn2.json").write_text(r2.text, encoding="utf-8")
    long_ar = ("قصة " * 120).strip()
    r = v.post("/v1/turns/text", headers={**text_mode, "X-Robot-Context": r2.json()["ctx"]},
               json={"text": "احك لي قصة " + long_ar[:300]})
    (OUT / "vercel_story_ar.json").write_text(r.text, encoding="utf-8")
    r = v.post("/v1/turns/text", headers=h, json={"text": "tell me a story"})  # speaker: sealed audio ticket
    (OUT / "vercel_audio_ticket.json").write_text(r.text, encoding="utf-8")
    groq_fixtures(reg, text_mode, h)
    print(f"wrote {len(list(OUT.glob('*.json')))} fixtures to {OUT}")


def _fake_groq():
    """A MOCKED Groq API (no network): chat answers and Orpheus-like 24 kHz WAV speech."""
    import io
    import wave

    import httpx

    def handler(req: httpx.Request) -> httpx.Response:
        body = json.loads(req.content)
        if req.url.path.endswith("/chat/completions"):
            ar = any("\u0600" <= ch <= "\u06ff" for ch in body["messages"][-1]["content"])
            text = ("السماء زرقاء لأن الهواء ينشر الضوء الأزرق أكثر من الألوان الأخرى." if ar else
                    "The sky looks blue because the air scatters blue sunlight more than other colors.")
            return httpx.Response(200, json={"model": body["model"], "choices": [
                {"index": 0, "finish_reason": "stop", "message": {"role": "assistant", "content": text}}],
                "usage": {"prompt_tokens": 400, "completion_tokens": 60}})
        n = int(0.02 * len(body["input"]) * 24000)
        buf = io.BytesIO()
        with wave.open(buf, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(24000)
            w.writeframes(b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * 330 * i / 24000)))
                                   for i in range(n)))
        return httpx.Response(200, content=buf.getvalue())
    return httpx.MockTransport(handler)


def groq_fixtures(reg, text_mode, h) -> None:
    """Groq gpt-oss + Orpheus (mocked API) on the serverless gateway, as the robot sees it,
    including the raw chunked HTTP body of the streamed speech captured from a real server."""
    import socket
    import threading
    import time

    import uvicorn

    from robot_gateway.providers.groq_tts import GroqOrpheusTTS
    from robot_gateway.providers.openai_compat import GROQ_BASE_URL, OpenAICompatibleLLM

    transport = _fake_groq()
    llm = OpenAICompatibleLLM(GROQ_BASE_URL, "openai/gpt-oss-120b", "gsk_fixture", "groq",
                              reasoning_effort="low", transport=transport)
    tts = GroqOrpheusTTS("gsk_fixture", transport=transport)
    app = create_app(Settings(), stt=MockSTT(), llm=llm, tts=tts, registry=reg, stateless=True,
                     context_secret=b"fixture-secret-0123456789abcdefghij")
    c = TestClient(app)
    r = c.post("/v1/turns/text", headers=h, json={"text": "why is the sky blue?"})  # speaker robot
    (OUT / "groq_speaker_en.json").write_text(r.text, encoding="utf-8")
    path = c.post("/v1/turns/text", headers=h, json={"text": "tell me a story"}).json()["audio"]["path"]
    r = c.post("/v1/turns/text", headers=text_mode, json={"text": "لماذا السماء زرقاء؟"})
    (OUT / "groq_text_ar.json").write_text(r.text, encoding="utf-8")

    server = uvicorn.Server(uvicorn.Config(app, host="127.0.0.1", port=0, log_level="warning"))
    th = threading.Thread(target=server.run, daemon=True)
    th.start()
    while not server.started:
        time.sleep(0.05)
    port = server.servers[0].sockets[0].getsockname()[1]
    with socket.create_connection(("127.0.0.1", port)) as s:
        s.sendall((f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer {TOKEN}\r\n"
                   "Connection: close\r\n\r\n").encode())
        raw = b""
        while chunk := s.recv(65536):
            raw += chunk
    server.should_exit = True
    th.join(5)
    head, body = raw.split(b"\r\n\r\n", 1)
    headers = head.decode().lower()
    assert "200 ok" in headers and "transfer-encoding: chunked" in headers and "rate=24000" in headers, headers
    stream_dir = ROOT / "firmware/test/test_http_body/fixtures"
    stream_dir.mkdir(parents=True, exist_ok=True)
    (stream_dir / "groq_tts_stream.bin").write_bytes(body)
    pcm = TestClient(app).get(path, headers=h).content  # the same audio, decoded by httpx
    (stream_dir / "groq_tts_stream.json").write_text(json.dumps(
        {"pcm_bytes": len(pcm), "byte_sum": sum(pcm) & 0xFFFFFFFF, "sample_rate": 24000}), encoding="utf-8")


if __name__ == "__main__":
    main()
