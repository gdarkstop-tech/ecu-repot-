import math
import struct

import pytest
from fastapi.testclient import TestClient

from robot_gateway.app import create_app
from robot_gateway.config import Settings
from robot_gateway.devices import DeviceRegistry, hash_token
from robot_gateway.providers.mock import MockLLM, MockSTT, MockTTS

TOKEN = "rbt_test_token_aaaaaaaaaaaaaaaa"
OTHER = "rbt_test_token_bbbbbbbbbbbbbbbb"


def tone(seconds: float, amp: int = 8000, rate: int = 16000) -> bytes:
    return b"".join(struct.pack("<h", int(amp * math.sin(2 * math.pi * 300 * i / rate)))
                    for i in range(int(seconds * rate)))


def make_client(settings: Settings | None = None, llm=None, stt=None, tts=None) -> TestClient:
    reg = DeviceRegistry(None, f"robot-1:{hash_token(TOKEN)},robot-2:{hash_token(OTHER)}")
    app = create_app(settings or Settings(), stt=stt or MockSTT(), llm=llm or MockLLM(), tts=tts or MockTTS(),
                     registry=reg)
    return TestClient(app)


@pytest.fixture
def client() -> TestClient:
    return make_client()


AUTH = {"Authorization": f"Bearer {TOKEN}"}
AUTH2 = {"Authorization": f"Bearer {OTHER}"}
L16 = {"Content-Type": "audio/L16;rate=16000;channels=1"}
