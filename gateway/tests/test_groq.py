"""Groq providers against a MOCKED Groq API (httpx.MockTransport): request shapes, response
parsing, WAV conversion, rate limits, retries and safe logging. No real Groq request is made
here; live behaviour is verified on the deployed gateway (docs/VERCEL_SETUP.md)."""
import array
import io
import json
import logging
import math
import re
import struct
import wave

import httpx
import pytest
from conftest import AUTH, OTHER, TOKEN
from fastapi.testclient import TestClient

from robot_gateway.app import create_app
from robot_gateway.audio import PEAK_LIMIT, decode_wav
from robot_gateway.config import Settings
from robot_gateway.devices import DeviceRegistry, hash_token
from robot_gateway.providers.base import ChatMessage, ProviderError
from robot_gateway.providers.factory import build_llm, build_tts
from robot_gateway.providers.groq_tts import GroqOrpheusTTS, clean_for_speech, split_for_speech
from robot_gateway.providers.mock import MockSTT
from robot_gateway.providers.openai_compat import GROQ_BASE_URL, OpenAICompatibleLLM

FAKE_KEY = "gsk_TEST-NOT-A-REAL-KEY-0000"
SECRET = b"groq-test-secret-0123456789abcdefghij"


# ------------------------------------------------------------------ fake Groq
def wav(seconds: float, rate: int = 24000, channels: int = 1, amp: float = 0.5, width: int = 2,
        streaming_header: bool = False) -> bytes:
    n = int(seconds * rate)
    frames = array.array("h", (int(amp * 32767 * math.sin(2 * math.pi * 440 * i / rate))
                               for i in range(n) for _ in range(channels)))
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(width)
        w.setframerate(rate)
        w.writeframes(frames.tobytes())
    data = buf.getvalue()
    if streaming_header:  # some TTS servers stream WAV with "unknown" sizes
        data = data[:4] + b"\xff\xff\xff\xff" + data[8:40] + b"\xff\xff\xff\xff" + data[44:]
    return data


class FakeGroq:
    """Records requests; answers like Groq's OpenAI-compatible API."""

    def __init__(self, chat=None, speech=None):
        self.requests: list[httpx.Request] = []
        self.chat = chat or (lambda body, n: httpx.Response(200, json=completion("The sky looks blue because air "
                                                                                 "scatters blue sunlight.")))
        self.speech = speech or (lambda body, n: httpx.Response(200, content=wav(0.02 * len(body["input"])),
                                                                headers={"content-type": "audio/wav"}))

    def __call__(self, req: httpx.Request) -> httpx.Response:
        self.requests.append(req)
        body = json.loads(req.content)
        n = len(self.requests)
        if req.url.path.endswith("/chat/completions"):
            return self.chat(body, n)
        if req.url.path.endswith("/audio/speech"):
            return self.speech(body, n)
        return httpx.Response(404, json={"error": {"type": "not_found", "message": "x"}})

    def bodies(self, path_end: str) -> list[dict]:
        return [json.loads(r.content) for r in self.requests if r.url.path.endswith(path_end)]


def completion(text: str, finish: str = "stop", **msg) -> dict:
    return {"id": "x", "model": "openai/gpt-oss-120b", "object": "chat.completion",
            "choices": [{"index": 0, "finish_reason": finish, "message": {"role": "assistant", "content": text, **msg}}],
            "usage": {"prompt_tokens": 420, "completion_tokens": 96, "total_tokens": 516}}


def rate_limited(retry_after: str = "30"):
    return httpx.Response(429, headers={"retry-after": retry_after},
                          json={"error": {"message": "Rate limit reached for model in organization org_SECRETORG "
                                                     "on requests per day (RPD): Limit 100, Used 100",
                                          "type": "tokens", "code": "rate_limit_exceeded"}})


def groq_llm(fake: FakeGroq, model: str = "openai/gpt-oss-120b") -> OpenAICompatibleLLM:
    return OpenAICompatibleLLM(GROQ_BASE_URL, model, FAKE_KEY, "groq",
                               params={"max_completion_tokens": 1024, "temperature": 0.6},
                               reasoning_effort="low", transport=httpx.MockTransport(fake))


def groq_tts(fake: FakeGroq, max_chunks: int = 2) -> GroqOrpheusTTS:
    return GroqOrpheusTTS(FAKE_KEY, max_chunks=max_chunks, transport=httpx.MockTransport(fake))


def gateway(llm=None, tts=None, stateless=True) -> TestClient:
    reg = DeviceRegistry(None, f"robot-1:{hash_token(TOKEN)},robot-2:{hash_token(OTHER)}")
    from robot_gateway.providers.mock import MockLLM, MockTTS
    return TestClient(create_app(Settings(), stt=MockSTT(), llm=llm or MockLLM(), tts=tts or MockTTS(),
                                 registry=reg, stateless=stateless, context_secret=SECRET))


# ------------------------------------------------------------------ language model
def test_gpt_oss_request_shape_and_parsing():
    fake = FakeGroq()
    r = groq_llm(fake).reply("SYSTEM", [ChatMessage("user", "hi"), ChatMessage("assistant", "hello")], "why?")
    req = fake.requests[0]
    assert str(req.url) == "https://api.groq.com/openai/v1/chat/completions"
    assert req.headers["authorization"] == f"Bearer {FAKE_KEY}"
    body = json.loads(req.content)
    assert body["model"] == "openai/gpt-oss-120b"
    assert body["include_reasoning"] is False and body["reasoning_effort"] == "low"
    assert body["max_completion_tokens"] == 1024 and body["temperature"] == 0.6
    assert [m["role"] for m in body["messages"]] == ["system", "user", "assistant", "user"]
    assert r.text.startswith("The sky looks blue") and (r.input_tokens, r.output_tokens) == (420, 96)
    assert r.model == "openai/gpt-oss-120b" and not r.refused


def test_other_groq_models_get_no_reasoning_parameters():
    fake = FakeGroq()
    groq_llm(fake, model="llama-3.3-70b-versatile").reply("S", [], "hi")
    body = fake.bodies("/chat/completions")[0]
    assert "reasoning_effort" not in body and "include_reasoning" not in body


def test_factory_defaults_to_groq_gpt_oss_when_a_groq_key_exists(monkeypatch):
    for k in ("LLM_PROVIDER", "STT_PROVIDER", "TTS_PROVIDER", "GROQ_LLM_MODEL"):
        monkeypatch.delenv(k, raising=False)
    monkeypatch.setenv("GROQ_API_KEY", FAKE_KEY)
    s = Settings.from_env()
    assert (s.llm_provider, s.stt_provider, s.groq_llm_model) == ("groq", "groq", "openai/gpt-oss-120b")
    assert s.tts_provider in ("mock", "espeak")  # the natural voice stays opt-in
    llm = build_llm(s)
    assert (llm.name, llm.model, llm.params["reasoning_effort"]) == ("groq", "openai/gpt-oss-120b", "low")
    monkeypatch.setenv("GROQ_LLM_MODEL", "openai/gpt-oss-20b")
    assert build_llm(Settings.from_env()).model == "openai/gpt-oss-20b"
    monkeypatch.setenv("LLM_PROVIDER", "anthropic")  # Anthropic stays available as an option
    assert build_llm(Settings.from_env()).name == "anthropic"
    monkeypatch.delenv("GROQ_API_KEY")
    monkeypatch.delenv("LLM_PROVIDER")
    assert Settings.from_env().llm_provider == "mock"  # no key: keyless demo, never a paid default


def test_rate_limit_short_wait_is_retried_once(monkeypatch):
    monkeypatch.setattr("robot_gateway.providers.openai_compat.time.sleep", lambda s: None)
    fake = FakeGroq(chat=lambda body, n: rate_limited("0.5") if n == 1 else httpx.Response(200, json=completion("ok")))
    assert groq_llm(fake).reply("S", [], "hi").text == "ok" and len(fake.requests) == 2


def test_rate_limit_long_wait_fails_fast_and_logs_safely(caplog):
    caplog.set_level(logging.INFO)
    fake = FakeGroq(chat=lambda body, n: rate_limited("30"))
    with pytest.raises(ProviderError) as e:
        groq_llm(fake).reply("S", [], "my secret question about dinosaurs")
    assert e.value.code == "llm_rate_limited" and e.value.retryable and len(fake.requests) == 1
    logged = caplog.text
    assert "status=429" in logged and "code=rate_limit_exceeded" in logged and "model=openai/gpt-oss-120b" in logged
    for private in (FAKE_KEY, "dinosaurs", "org_SECRETORG", "Rate limit reached"):
        assert private not in logged


@pytest.mark.parametrize("status,code,retryable", [(401, "llm_auth_failed", False), (404, "llm_model_not_found", False),
                                                    (400, "llm_http_400", False), (503, "llm_http_503", True)])
def test_error_mapping(status, code, retryable, monkeypatch):
    monkeypatch.setattr("robot_gateway.providers.openai_compat.time.sleep", lambda s: None)
    fake = FakeGroq(chat=lambda body, n: httpx.Response(status, json={"error": {"type": "x", "message": "m"}}))
    with pytest.raises(ProviderError) as e:
        groq_llm(fake).reply("S", [], "hi")
    assert (e.value.code, e.value.retryable) == (code, retryable)


def test_timeout_and_refusal():
    def boom(req):
        raise httpx.ReadTimeout("slow", request=req)
    llm = OpenAICompatibleLLM(GROQ_BASE_URL, "openai/gpt-oss-120b", FAKE_KEY, "groq", transport=httpx.MockTransport(boom))
    with pytest.raises(ProviderError) as e:
        llm.reply("S", [], "hi")
    assert e.value.code == "llm_timeout"
    fake = FakeGroq(chat=lambda b, n: httpx.Response(200, json=completion("", finish="content_filter")))
    assert groq_llm(fake).reply("S", [], "hi").refused


def test_open_question_reaches_groq_llm_in_english_and_arabic():
    fake = FakeGroq(chat=lambda body, n: httpx.Response(200, json=completion(
        "السماء زرقاء لأن الهواء ينشر الضوء الأزرق أكثر من غيره." if "زرقاء" in body["messages"][-1]["content"]
        else "The sky looks blue because air scatters blue sunlight the most.")))
    c = gateway(llm=groq_llm(fake))
    en = c.post("/v1/turns/text", headers={**AUTH, "X-Robot-Output": "text"}, json={"text": "Why is the sky blue?"}).json()
    assert en["source"] == "llm" and en["reply_text"].startswith("The sky looks blue")
    sent = fake.bodies("/chat/completions")[0]
    assert "Why is the sky blue?" in sent["messages"][-1]["content"]
    assert "English" in sent["messages"][0]["content"] and "under 25 words" in sent["messages"][0]["content"]
    ar = c.post("/v1/turns/text", headers={**AUTH, "X-Robot-Output": "text", "X-Robot-Context": en["ctx"]},
                json={"text": "لماذا السماء زرقاء؟"}).json()
    assert ar["source"] == "llm" and ar["lang"] == "ar" and ar["display"]["long_rtl"] is True
    sent = fake.bodies("/chat/completions")[1]
    assert "Arabic" in sent["messages"][0]["content"]
    assert [m["role"] for m in sent["messages"]] == ["system", "user", "assistant", "user"]  # memory kept


def test_tutor_and_safety_answer_without_calling_groq():
    fake = FakeGroq()
    c = gateway(llm=groq_llm(fake))
    assert c.post("/v1/turns/text", headers=AUTH, json={"text": "what is 7 plus 5?"}).json()["source"] == "math"
    r = c.post("/v1/turns/text", headers=AUTH, json={"text": "my phone number is 555 123 4567"}).json()
    assert r["source"] == "safety"
    assert fake.requests == []


def test_llm_rate_limit_gives_a_kind_answer_not_an_error():
    fake = FakeGroq(chat=lambda body, n: rate_limited("40"))
    c = gateway(llm=groq_llm(fake))
    r = c.post("/v1/turns/text", headers={**AUTH, "X-Robot-Output": "text"}, json={"text": "Why do cats purr?"})
    assert r.status_code == 200
    b = r.json()
    assert b["source"] == "fallback" and "llm_rate_limited" in b["warnings"] and "break" in b["reply_text"]
    assert b["ctx"]  # memory still returned


# ------------------------------------------------------------------ Orpheus text-to-speech
def test_speech_text_is_cleaned_and_split_under_200_characters():
    assert clean_for_speech("Hi [whisper] there *friend*") == "Hi there friend"
    text = ("Once upon a time, a little turtle named Tim wanted to see the sea. Every day he walked a little "
            "further, slow and steady. One morning he heard the waves! He smiled, because patience had brought "
            "him all the way to the beautiful blue water.")
    pieces = split_for_speech(text)
    assert all(len(p) <= 200 for p in pieces) and len(pieces) == 2
    assert pieces[0].endswith((".", "!")) and " ".join(pieces) == text
    ar = "كان يا ما كان، سلحفاة صغيرة اسمها سلمى. " * 8
    assert all(len(p) <= 200 for p in split_for_speech(ar))
    assert all(len(p) <= 200 for p in split_for_speech("x" * 450))  # no spaces: hard cut


def test_orpheus_request_shape_and_playable_pcm():
    fake = FakeGroq()
    sp = groq_tts(fake).synthesize("Hello! [cheerful] I am your robot friend.", "en")
    body = fake.bodies("/audio/speech")[0]
    assert str(fake.requests[0].url) == "https://api.groq.com/openai/v1/audio/speech"
    assert body == {"model": "canopylabs/orpheus-v1-english", "voice": "hannah",
                    "input": "Hello! I am your robot friend.", "response_format": "wav"}
    assert sp.sample_rate == 24000 and len(sp.pcm) % 2 == 0
    samples = array.array("h", sp.pcm)
    assert len(samples) == int(0.02 * len(body["input"]) * 24000)  # all audio kept, nothing resampled
    assert max(abs(s) for s in samples) <= PEAK_LIMIT


def test_arabic_uses_the_saudi_model_and_voice():
    fake = FakeGroq()
    groq_tts(fake).synthesize("مرحبا يا صديقي", "ar")
    assert fake.bodies("/audio/speech")[0]["model"] == "canopylabs/orpheus-arabic-saudi"
    assert fake.bodies("/audio/speech")[0]["voice"] == "lulwa"


@pytest.mark.parametrize("kind", ["stereo_48k", "float32", "streaming_header", "loud"])
def test_any_reasonable_wav_becomes_24k_mono_s16(kind):
    def speech(body, n):
        if kind == "stereo_48k":
            data = wav(0.5, rate=48000, channels=2)
        elif kind == "float32":
            pcm = array.array("f", (0.5 * math.sin(i / 7) for i in range(12000)))
            fmt = struct.pack("<HHIIHH", 3, 1, 24000, 24000 * 4, 4, 32)
            data = (b"RIFF" + struct.pack("<I", 36 + len(pcm) * 4) + b"WAVEfmt " + struct.pack("<I", 16) + fmt
                    + b"data" + struct.pack("<I", len(pcm) * 4) + pcm.tobytes())
        elif kind == "streaming_header":
            data = wav(0.5, streaming_header=True)
        else:
            data = wav(0.5, amp=1.0)
        return httpx.Response(200, content=data)
    sp = groq_tts(FakeGroq(speech=speech)).synthesize("Hello there.", "en")
    samples = array.array("h", sp.pcm)
    assert sp.sample_rate == 24000
    assert abs(len(samples) - 12000) <= 2                      # 0.5 s at 24 kHz
    assert 3000 < max(abs(s) for s in samples) <= PEAK_LIMIT    # audible, never clipped


def test_long_answers_are_capped_to_protect_the_daily_quota():
    fake = FakeGroq()
    tts = groq_tts(fake, max_chunks=2)
    tts.synthesize("This is a sentence that keeps going for a while. " * 12, "en")
    inputs = [b["input"] for b in fake.bodies("/audio/speech")]
    assert len(inputs) == 2 and all(len(i) <= 200 for i in inputs)


def test_tts_errors(monkeypatch, caplog):
    monkeypatch.setattr("robot_gateway.providers.openai_compat.time.sleep", lambda s: None)
    with pytest.raises(ProviderError) as e:
        groq_tts(FakeGroq(speech=lambda b, n: rate_limited("60"))).synthesize("Hello there.", "en")
    assert e.value.code == "tts_rate_limited"
    terms = lambda b, n: httpx.Response(400, json={"error": {"message": "accept terms", "type": "invalid_request_error",
                                                             "code": "model_terms_required"}})
    with pytest.raises(ProviderError) as e:
        groq_tts(FakeGroq(speech=terms)).synthesize("Hello there.", "en")
    assert e.value.code == "tts_terms_required" and not e.value.retryable
    with pytest.raises(ProviderError) as e:
        groq_tts(FakeGroq(speech=lambda b, n: httpx.Response(200, content=b"not audio"))).synthesize("Hi there.", "en")
    assert e.value.code == "tts_bad_audio"
    # a later piece failing keeps what was already synthesised
    partial = groq_tts(FakeGroq(speech=lambda b, n: httpx.Response(200, content=wav(0.3)) if n == 1
                                else rate_limited("60"))).synthesize("First sentence here. " * 15, "en")
    assert len(partial.pcm) == 2 * int(0.3 * 24000)
    assert FAKE_KEY not in caplog.text


def test_factory_builds_orpheus_with_configured_voices(monkeypatch):
    monkeypatch.setenv("GROQ_API_KEY", FAKE_KEY)
    monkeypatch.setenv("TTS_PROVIDER", "groq")
    monkeypatch.setenv("GROQ_TTS_VOICE_EN", "troy")
    monkeypatch.setenv("GROQ_TTS_MAX_CHUNKS", "3")
    tts = build_tts(Settings.from_env())
    assert (tts.name, tts.rate, tts.voices["en"], tts.voices["ar"], tts.max_chunks) == ("groq", 24000, "troy", "lulwa", 3)


# ------------------------------------------------------------------ through the gateway (as the robot)
def speak_turn(c: TestClient, text: str = "tell me a story", headers=AUTH) -> dict:
    r = c.post("/v1/turns/text", headers=headers, json={"text": text})
    assert r.status_code == 200
    return r.json()


def test_speaker_robot_gets_streamed_24k_speech_from_any_instance():
    fake = FakeGroq()
    a, b = gateway(tts=groq_tts(fake)), gateway(tts=groq_tts(fake))
    reply = speak_turn(a)
    audio = reply["audio"]
    assert audio["format"] == "pcm_s16le" and audio["sample_rate"] == 24000 and len(audio["path"]) <= 1023
    assert re.fullmatch(r"/v1/[A-Za-z0-9/_.-]+", audio["path"])
    assert reply["display"]["long_text"].startswith("Once upon a time")  # shown if speech fails
    assert fake.requests == []                                             # nothing synthesised yet
    got = b.get(audio["path"], headers=AUTH)                               # another instance
    assert got.status_code == 200 and got.headers["content-type"] == "audio/L16;rate=24000;channels=1"
    inputs = [x["input"] for x in fake.bodies("/audio/speech")]
    assert 1 <= len(inputs) <= 2 and all(len(i) <= 200 for i in inputs)
    expected = sum(int(0.02 * len(i) * 24000) for i in inputs) + int(0.12 * 24000) * (len(inputs) - 1)
    assert len(got.content) == 2 * expected                                # every piece + gaps, whole samples
    assert b.get(audio["path"], headers={"Authorization": f"Bearer {OTHER}"}).status_code == 404


def test_tts_rate_limit_on_audio_fetch_is_a_clear_429():
    c = gateway(tts=groq_tts(FakeGroq(speech=lambda b, n: rate_limited("60"))))
    reply = speak_turn(c, "tell me a story")
    r = c.get(reply["audio"]["path"], headers=AUTH)
    assert r.status_code == 429 and r.json() == {"error": "tts_rate_limited"}


def test_long_running_host_synthesises_once_and_serves_once():
    fake = FakeGroq()
    c = gateway(tts=groq_tts(fake), stateless=False)
    reply = speak_turn(c)
    assert reply["audio"]["sample_rate"] == 24000 and reply["audio"]["bytes"] > 0
    first = c.get(reply["audio"]["path"], headers=AUTH)
    assert first.status_code == 200 and len(first.content) == reply["audio"]["bytes"]
    assert c.get(reply["audio"]["path"], headers=AUTH).status_code == 404  # one-time


def test_text_only_robot_never_calls_groq_tts():
    fake = FakeGroq()
    r = speak_turn(gateway(tts=groq_tts(fake)), headers={**AUTH, "X-Robot-Output": "text"})
    assert r["audio"] is None and fake.bodies("/audio/speech") == []


def test_health_shows_models_but_no_key(monkeypatch):
    monkeypatch.setenv("GROQ_API_KEY", FAKE_KEY)
    for k, v in {"LLM_PROVIDER": "groq", "STT_PROVIDER": "groq", "TTS_PROVIDER": "groq"}.items():
        monkeypatch.setenv(k, v)
    app = create_app(Settings.from_env(), registry=DeviceRegistry(None, f"robot-1:{hash_token(TOKEN)}"),
                     stateless=True, context_secret=SECRET)
    r = TestClient(app).get("/healthz")
    body = r.json()
    assert r.status_code == 200 and body["ok"] is True
    assert (body["stt"], body["llm"], body["tts"]) == ("groq", "groq", "groq")
    assert body["llm_model"] == "openai/gpt-oss-120b" and "orpheus" in body["tts_model"]
    assert FAKE_KEY not in r.text and body["warnings"] == []
