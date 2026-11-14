"""Exercises the real Anthropic SDK request/response path against a mock transport.
No API key or network: this verifies the request we build and how we read the response."""
import json

import anthropic
import httpx2
import pytest

from robot_gateway.providers.anthropic_llm import AnthropicLLM
from robot_gateway.providers.base import ChatMessage, ProviderError


def make(handler, model="claude-haiku-5-5"):
    client = anthropic.Anthropic(api_key="test-key", base_url="https://api.test",
                                 http_client=httpx2.Client(transport=httpx2.MockTransport(handler)), max_retries=0)
    return AnthropicLLM(model=model, client=client)


def message(text="Hello!", stop="end_turn", model="claude-haiku-5-5"):
    return {"id": "msg_1", "type": "message", "role": "assistant", "model": model,
            "content": [{"type": "thinking", "thinking": "", "signature": "sig"}, {"type": "text", "text": text}],
            "stop_reason": stop, "stop_sequence": None, "usage": {"input_tokens": 410, "output_tokens": 37}}


def test_default_is_haiku_low_effort_without_fallbacks():
    seen = {}

    def handler(req):
        seen["body"] = json.loads(req.content)
        seen["beta"] = req.headers.get("anthropic-beta", "")
        seen["path"] = req.url.path
        return httpx2.Response(200, json=message("Cats purr when happy."))

    r = make(handler).reply("SYSTEM", [ChatMessage("user", "hi"), ChatMessage("assistant", "hello")], "why purr?")
    b = seen["body"]
    assert r.text == "Cats purr when happy." and not r.refused  # text read by block type, after thinking
    assert (r.input_tokens, r.output_tokens) == (410, 37)
    assert b["model"] == "claude-haiku-5-5"
    assert b["output_config"] == {"effort": "low"}
    assert b["max_tokens"] == 2048
    assert "fallbacks" not in b and "server-side-fallback" not in seen["beta"]  # Haiku 5.5: no fallback
    assert seen["path"] == "/v1/messages"
    assert b["system"][0]["cache_control"] == {"type": "ephemeral"}
    assert [m["role"] for m in b["messages"]] == ["user", "assistant", "user"]
    assert "thinking" not in b and "temperature" not in b


def test_fallbacks_only_for_models_that_support_them():
    seen = {}

    def handler(req):
        seen["body"] = json.loads(req.content)
        seen["beta"] = req.headers.get("anthropic-beta", "")
        return httpx2.Response(200, json=message("ok", model="claude-opus-5-5"))

    make(handler, model="claude-opus-5-5").reply("S", [], "x")
    assert seen["body"]["fallbacks"] == "default" and "server-side-fallback-2026-07-01" in seen["beta"]


def test_refusal_is_reported():
    r = make(lambda req: httpx2.Response(200, json=message("", "refusal"))).reply("S", [], "x")
    assert r.refused


def test_auth_error_is_not_retryable():
    def handler(req):
        return httpx2.Response(401, json={"type": "error", "error": {"type": "authentication_error", "message": "bad"}})

    with pytest.raises(ProviderError) as e:
        make(handler).reply("S", [], "x")
    assert e.value.code == "llm_auth_failed" and not e.value.retryable


def test_overload_is_retryable():
    def handler(req):
        return httpx2.Response(529, json={"type": "error", "error": {"type": "overloaded_error", "message": "busy"}})

    with pytest.raises(ProviderError) as e:
        make(handler).reply("S", [], "x")
    assert e.value.retryable
