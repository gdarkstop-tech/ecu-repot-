from conftest import AUTH, AUTH2, L16, make_client, tone

from robot_gateway.config import Settings


def test_health_has_no_secrets(client):
    body = client.get("/healthz").json()
    assert body["ok"] and body["devices"] == 2
    assert "key" not in str(body).lower()


def test_auth_required_everywhere(client):
    assert client.post("/v1/turns/text", json={"text": "hi"}).status_code == 401
    assert client.post("/v1/turns/text", json={"text": "hi"},
                       headers={"Authorization": "Bearer wrong_token_xxxxxxxx"}).status_code == 401
    assert client.post("/v1/turns", content=tone(1), headers=L16).status_code == 401
    assert client.get("/v1/turns/abcdefghijk/audio").status_code == 401


def test_rejects_bad_audio(client):
    assert client.post("/v1/turns", content=tone(1), headers={**AUTH, "Content-Type": "audio/wav"}).status_code == 415
    stereo = {**AUTH, "Content-Type": "audio/L16;rate=16000;channels=2"}
    assert client.post("/v1/turns", content=tone(1), headers=stereo).status_code == 415
    assert client.post("/v1/turns", content=b"\x00\x00" * 100, headers={**AUTH, **L16}).status_code == 400
    too_long = tone(16)  # limit 15 s
    assert client.post("/v1/turns", content=too_long, headers={**AUTH, **L16}).status_code == 413


def test_silence_is_ignored(client):
    r = client.post("/v1/turns", content=b"\x00\x00" * 16000, headers={**AUTH, **L16})
    assert r.status_code == 200 and r.json()["ignore"] is True


def test_demo_audio_turn_is_honest_and_audio_is_one_time_and_device_bound(client):
    r = client.post("/v1/turns", content=tone(1.2), headers={**AUTH, **L16})
    b = r.json()
    assert r.status_code == 200 and b["ignore"] is False
    assert "demo mode" in b["reply_text"]
    path = b["audio"]["path"]
    assert path.startswith("/v1/turns/") and b["audio"]["format"] == "pcm_s16le"
    assert client.get(path, headers=AUTH2).status_code == 404  # another robot cannot fetch it
    a = client.get(path, headers=AUTH)
    assert a.status_code == 200 and len(a.content) == b["audio"]["bytes"]
    assert a.headers["content-type"].startswith("audio/L16;rate=16000")
    assert client.get(path, headers=AUTH).status_code == 404  # one-time


def test_rate_limit():
    c = make_client(Settings(turns_per_minute=3))
    codes = [c.post("/v1/turns/text", headers=AUTH, json={"text": "hello"}).status_code for _ in range(5)]
    assert codes == [200, 200, 200, 429, 429]


def test_math_is_computed_and_displayed(client):
    b = client.post("/v1/turns/text", headers=AUTH, json={"text": "what is 12 times 12?"}).json()
    assert "144" in b["reply_text"]
    assert b["display"]["content"] == {"kind": "math", "value": "12 x 12 = 144", "caption": ""}


def test_arabic_reply_is_shaped_for_oled(client):
    b = client.post("/v1/turns/text", headers=AUTH, json={"text": "احك لي قصة"}).json()
    assert b["lang"] == "ar" and b["display"]["rtl"] is True
    oled = b["display"]["oled_text"]
    assert any(0xFE70 <= ord(ch) <= 0xFEFF for ch in oled)  # presentation forms = pre-shaped
    assert len(oled.encode()) <= 150


def test_debug_transcript_only_when_enabled():
    c = make_client()
    r = c.post("/v1/turns", content=tone(1.2), headers={**AUTH, **L16, "X-Debug-Transcript": "what is 2 plus 2"})
    assert "demo mode" in r.json()["reply_text"]  # header ignored by default
    c2 = make_client(Settings(allow_debug_transcript=True))
    r2 = c2.post("/v1/turns", content=tone(1.2), headers={**AUTH, **L16, "X-Debug-Transcript": "what is 2 plus 2"})
    assert "4" in r2.json()["reply_text"]


def test_text_validation(client):
    assert client.post("/v1/turns/text", headers=AUTH, content=b"not json").status_code == 400
    assert client.post("/v1/turns/text", headers=AUTH, json={"text": ""}).status_code == 400
    assert client.post("/v1/turns/text", headers=AUTH, json={"text": "x" * 600}).status_code == 400


def test_reply_fits_robot_limits(client):
    for text in ["tell me a story", "احك لي قصة", "let's learn arabic words", "quiz"]:
        b = client.post("/v1/turns/text", headers=AUTH, json={"text": text}).json()
        import json as _j
        assert len(_j.dumps(b).encode()) < 8192  # robot rejects larger replies
        c = b["display"]["content"]
        if c:
            assert len(c["value"].encode()) <= 32
        assert b["emotion"] in {"happy", "sad", "surprised", "idle", "thinking"}
