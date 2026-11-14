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
    print(f"wrote {len(list(OUT.glob('*.json')))} fixtures to {OUT}")


if __name__ == "__main__":
    main()
