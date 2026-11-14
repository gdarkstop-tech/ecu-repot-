"""Exercises the real Anthropic SDK request/response path against a mock transport.
No API key or network: this verifies the request we build and how we read the response."""
import json

import anthropic
import httpx2
import pytest

from robot_gateway.providers.anthropic_llm import AnthropicLLM
from robot_gateway.providers.base import ChatMessage, ProviderError


def make(handler):
    client = anthropic.Anthropic(api_key="test-key", base_url="https://api.test",
                                 http_client=httpx2.Client(transport=httpx2.MockTransport(handler)), max_retries=0)
    return AnthropicLLM(client=client)


def message(text="Hello!", stop="end_turn"):
    return {"id": "msg_1", "type": "message", "role": "assistant", "model": "claude-opus-5-5",
            "content": [{"type": "text", "text": text}], "stop_reason": stop, "stop_sequence": None,
            "usage": {"input_tokens": 10, "output_tokens": 5}}


def test_request_shape_and_text():
    seen = {}

    def handler(req):
        seen["body"] = json.loads(req.content)
        seen["beta"] = req.headers.get("anthropic-beta", "")
        return httpx2.Response(200, json=message("Cats purr when happy."))

    r = make(handler).reply("SYSTEM", [ChatMessage("user", "hi"), ChatMessage("assistant", "hello")], "why purr?")
    b = seen["body"]
    assert r.text == "Cats purr when happy." and not r.refused
    assert b["model"] == "claude-opus-5-5"
    assert b["output_config"] == {"effort": "low"}
    assert b["fallbacks"] == "default" and "server-side-fallback-2026-07-01" in seen["beta"]
    assert b["system"][0]["cache_control"] == {"type": "ephemeral"}
    assert [m["role"] for m in b["messages"]] == ["user", "assistant", "user"]
    assert "thinking" not in b  # thinking cannot be disabled on Opus 5.5; effort controls depth


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
