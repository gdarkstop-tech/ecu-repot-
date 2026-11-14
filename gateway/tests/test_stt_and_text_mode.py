import json

import httpx
import pytest
from conftest import AUTH, L16, make_client, tone

from robot_gateway.config import Settings
from robot_gateway.providers.base import ProviderError, Transcript
from robot_gateway.providers.factory import build_stt
from robot_gateway.providers.openai_compat import OpenAICompatibleSTT
from robot_gateway.textutil import is_probable_hallucination


# ------------------------------------------------------------------ STT adapter (mock HTTP)
def stt_with(handler, fmt="verbose_json"):
    return OpenAICompatibleSTT("https://api.groq.com/openai/v1", "whisper-large-v3-turbo", "gsk_test", fmt,
                               name="groq", transport=httpx.MockTransport(handler))


def test_groq_request_shape_and_language():
    seen = {}

    def handler(req):
        seen["url"] = str(req.url)
        seen["auth"] = req.headers["authorization"]
        seen["body"] = req.content
        return httpx.Response(200, json={"text": " كم سبعة زائد خمسة؟ ", "language": "arabic",
                                         "segments": [{"text": "كم سبعة زائد خمسة؟", "no_speech_prob": 0.01,
                                                       "avg_logprob": -0.2}]})

    tr = stt_with(handler).transcribe(tone(1.0), 16000, "ar")
    assert seen["url"] == "https://api.groq.com/openai/v1/audio/transcriptions"
    assert seen["auth"] == "Bearer gsk_test"
    body = seen["body"]
    for part in (b'name="model"', b"whisper-large-v3-turbo", b'name="language"', b"ar", b"verbose_json",
                 b'filename="utterance.wav"', b"RIFF"):
        assert part in body
    assert tr.text == "كم سبعة زائد خمسة؟" and tr.language == "arabic"


def test_no_speech_segments_are_dropped():
    def handler(req):
        return httpx.Response(200, json={"text": "Thank you.", "segments": [
            {"text": "Thank you.", "no_speech_prob": 0.92, "avg_logprob": -1.4}]})

    assert stt_with(handler).transcribe(tone(1.0), 16000, None).text == ""


def test_stt_errors_map_to_provider_errors():
    for code, retry in ((401, False), (429, True), (503, True)):
        with pytest.raises(ProviderError) as e:
            stt_with(lambda req, c=code: httpx.Response(c, json={})).transcribe(tone(1), 16000, None)
        assert e.value.retryable is retry


def test_factory_presets(monkeypatch):
    monkeypatch.setenv("GROQ_API_KEY", "gsk_x")
    stt = build_stt(Settings(stt_provider="groq"))
    assert stt.base_url == "https://api.groq.com/openai/v1" and stt.model == "whisper-large-v3-turbo"
    assert stt.response_format == "verbose_json"
    monkeypatch.delenv("GROQ_API_KEY")
    with pytest.raises(ProviderError):
        build_stt(Settings(stt_provider="groq"))  # no key: refuse to start instead of failing per turn


def test_hallucination_filter():
    assert is_probable_hallucination("Thanks for watching!")
    assert is_probable_hallucination("ترجمة نانسي قنقر")
    assert not is_probable_hallucination("thank you robot")
    assert not is_probable_hallucination("what is seven plus five")
    assert not is_probable_hallucination("ما هو لون السماء")


# ------------------------------------------------------------------ pipeline behaviour
class CountingSTT:
    name = "counting"

    def __init__(self, text="what is 2 plus 3"):
        self.calls, self.text = 0, text

    def transcribe(self, pcm, rate, hint):
        self.calls += 1
        return Transcript(self.text, None)


def test_quiet_audio_never_reaches_paid_stt():
    stt = CountingSTT()
    c = make_client(stt=stt)
    r = c.post("/v1/turns", content=tone(1.0, amp=60), headers={**AUTH, **L16}).json()
    assert r["ignore"] is True and stt.calls == 0
    r = c.post("/v1/turns", content=tone(1.0), headers={**AUTH, **L16}).json()
    assert stt.calls == 1 and "5" in r["reply_text"]


def test_hallucinated_transcript_is_ignored():
    c = make_client(stt=CountingSTT("Thanks for watching!"))
    assert c.post("/v1/turns", content=tone(1.0), headers={**AUTH, **L16}).json()["ignore"] is True


class SpyTTS:
    name = "spy"

    def __init__(self):
        self.calls = 0

    def synthesize(self, text, lang):
        self.calls += 1
        raise AssertionError("TTS must not run in text-only mode")


def test_text_only_mode_skips_tts_and_returns_full_text():
    tts = SpyTTS()
    c = make_client(tts=tts)
    h = {**AUTH, "X-Robot-Output": "text"}
    b = c.post("/v1/turns/text", headers=h, json={"text": "tell me a story"}).json()
    assert tts.calls == 0 and b["audio"] is None
    d = b["display"]
    assert d["long_text"].startswith("Once upon a time") and d["long_rtl"] is False
    assert len(d["long_text"].encode()) <= 600
    b = c.post("/v1/turns/text", headers=h, json={"text": "احك لي قصة"}).json()
    d = b["display"]
    assert d["long_rtl"] is True and len(d["long_text"].encode()) <= 600
    assert any(0xFE70 <= ord(ch) <= 0xFEFF for ch in d["long_text"])  # shaped for the OLED


def test_long_text_is_trimmed_at_a_word_with_ellipsis():
    from robot_gateway.textutil import to_oled

    ar = " ".join(["كلمة"] * 200)
    out, rtl = to_oled(ar, 600)
    assert rtl and len(out.encode()) <= 600 and "..." in out
    en = " ".join(["word"] * 300)
    out, rtl = to_oled(en, 600)
    assert not rtl and len(out.encode()) <= 600 and out.endswith("word...")


def test_text_only_mode_asks_model_for_short_answers():
    from robot_gateway.providers.base import LlmResult

    class FakeLLM:
        name = "fake"

        def __init__(self):
            self.system = ""

        def reply(self, system, history, user):
            self.system = system
            return LlmResult("Cats purr when they feel safe and happy.", model="claude-haiku-5-5",
                             input_tokens=400, output_tokens=30)

    llm = FakeLLM()
    c = make_client(llm=llm)
    b = c.post("/v1/turns/text", headers={**AUTH, "X-Robot-Output": "text"}, json={"text": "why do cats purr?"}).json()
    assert "Keep it under 25 words" in llm.system
    assert b["display"]["long_text"] == "Cats purr when they feel safe and happy."
    c.post("/v1/turns/text", headers=AUTH, json={"text": "why do dogs bark?"})
    assert "Keep it under 25 words" not in llm.system  # only for screen-only robots


def test_reply_size_with_long_text_fits_robot():
    c = make_client()
    for t in ["احك لي قصة", "tell me a story", "a life lesson"]:
        b = c.post("/v1/turns/text", headers={**AUTH, "X-Robot-Output": "text"}, json={"text": t})
        assert len(b.content) < 8192
