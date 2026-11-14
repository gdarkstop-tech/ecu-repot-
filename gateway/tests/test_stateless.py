"""Serverless (Vercel) behaviour: nothing may depend on process memory or local files.

Two independent app instances stand in for two Vercel instances: they share only the secret
(an environment variable on the host), never memory. Requests alternate between them.
"""
import os
import re
import subprocess
import sys
import textwrap
from pathlib import Path

from conftest import AUTH, AUTH2, OTHER, TOKEN, tone
from fastapi.testclient import TestClient

from robot_gateway.app import create_app
from robot_gateway.config import Settings
from robot_gateway.devices import DeviceRegistry, hash_token
from robot_gateway.memory import ContextCodec
from robot_gateway.providers.base import LlmResult
from robot_gateway.providers.mock import MockLLM, MockSTT, MockTTS
from robot_gateway.sealing import Sealer, resolve_context_secret
from robot_gateway.speech import TicketSpeech
from robot_gateway.tutor.engine import Content, Pending, Session

SECRET = b"test-secret-for-two-instances-0123456789"
TEXT = {**AUTH, "X-Robot-Output": "text"}
SAFE_AUDIO_PATH = re.compile(r"/v1/[A-Za-z0-9/_.-]+")  # the firmware's isSafeAudioPath rules


def instance(secret: bytes = SECRET, llm=None, tts=None) -> TestClient:
    reg = DeviceRegistry(None, f"robot-1:{hash_token(TOKEN)},robot-2:{hash_token(OTHER)}")
    app = create_app(Settings(), stt=MockSTT(), llm=llm or MockLLM(), tts=tts or MockTTS(), registry=reg,
                     stateless=True, context_secret=secret)
    return TestClient(app)


def solve(question: str) -> str:
    m = re.search(r"(\d+) (plus|minus|times) (\d+)", question)
    a, op, b = int(m.group(1)), m.group(2), int(m.group(3))
    return str(a + b if op == "plus" else a - b if op == "minus" else a * b)


def turn(c: TestClient, text: str, ctx: str | None = None, headers=TEXT) -> dict:
    h = dict(headers)
    if ctx:
        h["X-Robot-Context"] = ctx
    r = c.post("/v1/turns/text", headers=h, json={"text": text})
    assert r.status_code == 200, r.text
    return r.json()


def test_tutor_state_follows_the_robot_across_instances():
    a, b = instance(), instance()
    r1 = turn(a, "let's do math")
    assert r1["ctx"] and len(r1["ctx"]) <= 2400
    r2 = turn(b, solve(r1["reply_text"]), r1["ctx"])   # answered on the OTHER instance
    assert r2["reply_text"].split("!")[0] in ("Correct", "Yes", "That's right")
    r3 = turn(a, solve(r2["reply_text"]), r2["ctx"])   # and back again
    assert r3["reply_text"].split("!")[0] in ("Correct", "Yes", "That's right")


def test_without_the_token_an_instance_remembers_nothing():
    a, b = instance(), instance()
    r1 = turn(a, "let's do math")
    r2 = turn(b, solve(r1["reply_text"]))  # no X-Robot-Context: fresh conversation
    assert not r2["reply_text"].startswith(("Correct", "Yes", "That's right"))


class RecordingLLM:
    name = "recording"

    def __init__(self):
        self.histories = []

    def reply(self, system, history, user_text):
        self.histories.append([(m.role, m.text) for m in history])
        return LlmResult(text="The sky looks blue because air scatters blue light.", model="fake",
                         input_tokens=10, output_tokens=10)


def test_model_history_travels_in_the_token():
    llm_a, llm_b = RecordingLLM(), RecordingLLM()
    a, b = instance(llm=llm_a), instance(llm=llm_b)
    r1 = turn(a, "why is the sky blue?")
    turn(b, "and why is the sunset red?", r1["ctx"])
    assert llm_a.histories == [[]]
    assert llm_b.histories[0] == [("user", "why is the sky blue?"),
                                  ("assistant", "The sky looks blue because air scatters blue light.")]


def test_tampered_foreign_and_wrong_key_tokens_start_fresh():
    a = instance()
    r1 = turn(a, "let's do math")
    answer = solve(r1["reply_text"])
    ctx = r1["ctx"]
    flipped = ctx[:20] + ("A" if ctx[20] != "A" else "B") + ctx[21:]
    for bad in (flipped, ctx[:-4], "not-a-token", "x" * 5000):
        assert not turn(a, answer, bad)["reply_text"].startswith(("Correct", "Yes", "That's right"))
    # robot-2 cannot use robot-1's context
    r = turn(a, answer, ctx, headers={**AUTH2, "X-Robot-Output": "text"})
    assert not r["reply_text"].startswith(("Correct", "Yes", "That's right"))
    # an instance with another secret cannot read it
    assert not turn(instance(b"another-secret-entirely-0123456789abcdef"), answer, ctx)["reply_text"].startswith(
        ("Correct", "Yes", "That's right"))


def test_token_expires_and_round_trips_all_tutor_state():
    codec = ContextCodec(Sealer(SECRET, "ctx"), ttl_seconds=1800)
    s = Session("robot-1", mode="quiz", index=3, level=2, streak=1, misses=1, quiz_asked=2, quiz_correct=1)
    s.pending = Pending(expected_number=None, accepted={"أحمر", "red"}, answer_text="red", attempts=1,
                        nudged=True, content_answer=Content("word", "RED", "أحمر"))
    tok = codec.encode(s, now=1_000_000)
    back = codec.decode(tok, "robot-1", now=1_000_000 + 1799)
    assert back is not None and (back.mode, back.index, back.level, back.quiz_asked) == ("quiz", 3, 2, 2)
    assert back.pending.accepted == {"أحمر", "red"} and back.pending.content_answer.caption == "أحمر"
    assert codec.decode(tok, "robot-1", now=1_000_000 + 1801) is None   # expired
    assert codec.decode(tok, "robot-2", now=1_000_000) is None          # other device
    s.pending = Pending(expected_number=__import__("fractions").Fraction(7, 2), answer_text="3.5")
    assert codec.decode(codec.encode(s), "robot-1").pending.expected_number == __import__("fractions").Fraction(7, 2)


def test_token_size_is_capped_by_dropping_the_oldest_exchanges():
    llm = RecordingLLM()
    llm.reply = lambda system, history, user: LlmResult(text="جواب طويل " * 40, model="fake")
    a = instance(llm=llm)
    ctx = None
    for i in range(12):
        r = turn(a, f"سؤال رقم {i} عن الفضاء والنجوم والكواكب البعيدة", ctx)
        ctx = r["ctx"]
        assert 0 < len(ctx) <= 2400
    s = ContextCodec(Sealer(SECRET, "ctx"), 1800).decode(ctx, "robot-1")
    assert s.history and s.history[0].role == "user" and "11" in s.history[-2].text  # newest kept


def test_ignored_turn_has_no_token_so_the_robot_keeps_its_own():
    a = instance()
    r = a.post("/v1/turns", content=b"\x00\x00" * 16000,
               headers={**TEXT, "Content-Type": "audio/L16;rate=16000;channels=1"})
    assert r.status_code == 200 and r.json()["ignore"] is True and "ctx" not in r.json()


def test_audio_ticket_works_on_another_instance_and_only_for_its_robot():
    a, b = instance(), instance()
    r = a.post("/v1/turns/text", headers=AUTH, json={"text": "tell me a story"}).json()
    path = r["audio"]["path"]
    assert SAFE_AUDIO_PATH.fullmatch(path) and ".." not in path and len(path) <= 1023
    assert r["audio"]["format"] == "pcm_s16le" and r["audio"]["sample_rate"] == 16000
    got = b.get(path, headers=AUTH)
    assert got.status_code == 200 and got.headers["content-type"].startswith("audio/L16;rate=16000")
    assert int(got.headers["content-length"]) == len(got.content) > 1000
    assert b.get(path, headers=AUTH2).status_code == 404            # bound to robot-1
    assert b.get(path[:-12] + "AAAA/audio", headers=AUTH).status_code == 404
    assert instance(b"another-secret-entirely-0123456789abcdef").get(path, headers=AUTH).status_code == 404


def test_audio_ticket_expires_and_long_text_still_fits_the_robot_path_limit():
    sp = TicketSpeech(MockTTS(), Sealer(SECRET, "audio"), ttl_seconds=120)
    info = sp.deliver("robot-1", "word " * 2000, "en", now=1000)
    ticket = info["path"].split("/")[3]
    assert len(info["path"]) <= 1023
    assert sp.fetch(ticket, "robot-1", now=1100) is not None
    assert sp.fetch(ticket, "robot-1", now=1121) is None


def test_health_reports_stateless_mode():
    body = instance().get("/healthz").json()
    assert body["stateless"] is True and body["memory"] == "context" and body["context_key"] == "provided"


def test_stateless_is_automatic_on_vercel(monkeypatch):
    monkeypatch.delenv("GATEWAY_STATELESS", raising=False)
    monkeypatch.setenv("VERCEL", "1")
    assert Settings.from_env().stateless is True
    monkeypatch.setenv("GATEWAY_STATELESS", "0")
    assert Settings.from_env().stateless is False
    monkeypatch.delenv("VERCEL")
    monkeypatch.delenv("GATEWAY_STATELESS")
    assert Settings.from_env().stateless is False


def test_context_secret_resolution():
    assert resolve_context_secret({"CONTEXT_SECRET": "s" * 32})[1] == "CONTEXT_SECRET"
    k1, src, warn = resolve_context_secret({"CONTEXT_SECRET": "short", "ANTHROPIC_API_KEY": "sk-1"})
    assert src == "derived" and "shorter" in warn
    assert resolve_context_secret({"ANTHROPIC_API_KEY": "sk-1"})[0] == k1   # same on every instance
    assert resolve_context_secret({})[1] == "random"


def test_missing_provider_key_does_not_crash_the_app(monkeypatch):
    monkeypatch.delenv("GROQ_API_KEY", raising=False)
    monkeypatch.delenv("STT_API_KEY", raising=False)
    monkeypatch.setenv("STT_PROVIDER", "groq")
    app = create_app(Settings.from_env(), registry=DeviceRegistry(None, f"robot-1:{hash_token(TOKEN)}"),
                     stateless=True, context_secret=SECRET)
    c = TestClient(app)
    h = c.get("/healthz")
    assert h.status_code == 503 and h.json()["ok"] is False and h.json()["errors"][0].startswith("stt:")
    r = c.post("/v1/turns", content=tone(1.0), headers={**TEXT, "Content-Type": "audio/L16;rate=16000;channels=1"})
    assert r.status_code == 500 and r.json()["error"]


def test_vercel_entrypoint_writes_no_files(tmp_path):
    """Run the real Vercel entrypoint (index.py) and a conversation in a fresh interpreter with an
    audit hook that records every file opened for writing."""
    gateway = Path(__file__).resolve().parents[1]
    script = textwrap.dedent(f"""
        import os, sys
        sys.path.insert(0, {str(gateway)!r})
        writes = []
        WRITE = os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_APPEND | os.O_TRUNC
        def hook(event, args):
            if event == "open":
                path, mode, flags = args
                if (isinstance(mode, str) and any(c in mode for c in "wax+")) or (flags or 0) & WRITE:
                    writes.append(str(path))
            elif event in ("os.remove", "os.rename", "os.mkdir", "shutil.rmtree"):
                writes.append(event + ":" + str(args[0]))
        from fastapi.testclient import TestClient
        import index                                # the module Vercel imports
        sys.addaudithook(hook)
        c = TestClient(index.app)
        h = {{"Authorization": "Bearer {TOKEN}", "X-Robot-Output": "text"}}
        assert c.get("/healthz").json()["stateless"] is True
        r = c.post("/v1/turns/text", headers=h, json={{"text": "let's do math"}}).json()
        h["X-Robot-Context"] = r["ctx"]
        r = c.post("/v1/turns/text", headers=h, json={{"text": "7"}})
        assert r.status_code == 200
        print("WRITES", writes)
    """)
    env = {k: v for k, v in os.environ.items() if not k.endswith("_API_KEY")}
    env.update(DEVICE_TOKEN_HASHES=f"robot-1:{hash_token(TOKEN)}", CONTEXT_SECRET="c" * 40,
               PYTHONDONTWRITEBYTECODE="1", STT_PROVIDER="mock", LLM_PROVIDER="mock", TTS_PROVIDER="mock")
    out = subprocess.run([sys.executable, "-c", script], cwd=tmp_path, env=env, capture_output=True, text=True,
                         timeout=60)
    assert out.returncode == 0, out.stderr[-2000:]
    assert "WRITES []" in out.stdout, out.stdout
