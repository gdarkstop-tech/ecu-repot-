"""Claude as the conversation model, via the official Anthropic Python SDK.

The API key is read by the SDK from ANTHROPIC_API_KEY (never logged or returned).
Defaults: Claude Opus 5.5 at low effort (short spoken replies), with server-side refusal
fallbacks enabled so a declined request is retried on a fallback model in the same call.
"""
from __future__ import annotations

import logging

import anthropic

from .base import ChatMessage, LlmResult, ProviderError

log = logging.getLogger("gateway.anthropic")


class AnthropicLLM:
    name = "anthropic"

    def __init__(self, model: str = "claude-opus-5-5", effort: str = "low", fallbacks: bool = True,
                 client: anthropic.Anthropic | None = None, max_tokens: int = 4096) -> None:
        # A short timeout: a child is waiting. The SDK retries 429/5xx/connection errors itself.
        self.client = client or anthropic.Anthropic(timeout=25.0, max_retries=1)
        self.model = model
        self.effort = effort
        self.fallbacks = fallbacks
        self.max_tokens = max_tokens

    def reply(self, system: str, history: list[ChatMessage], user_text: str) -> LlmResult:
        messages = [{"role": m.role, "content": m.text} for m in history]
        messages.append({"role": "user", "content": user_text})
        kwargs = dict(
            model=self.model,
            max_tokens=self.max_tokens,
            # The stable system prompt is cached; per-turn context is in the user message.
            system=[{"type": "text", "text": system, "cache_control": {"type": "ephemeral"}}],
            messages=messages,
            output_config={"effort": self.effort},
        )
        try:
            if self.fallbacks:
                response = self.client.beta.messages.create(
                    betas=["server-side-fallback-2026-07-01"], fallbacks="default", **kwargs
                )
            else:
                response = self.client.messages.create(**kwargs)
        except anthropic.AuthenticationError as exc:
            raise ProviderError("llm_auth_failed", retryable=False) from exc
        except anthropic.PermissionDeniedError as exc:
            raise ProviderError("llm_permission_denied", retryable=False) from exc
        except anthropic.BadRequestError as exc:
            log.warning("anthropic bad request: %s", exc.message)
            raise ProviderError("llm_bad_request", retryable=False) from exc
        except anthropic.RateLimitError as exc:
            raise ProviderError("llm_rate_limited") from exc
        except anthropic.APIStatusError as exc:
            raise ProviderError(f"llm_http_{exc.status_code}") from exc
        except anthropic.APIConnectionError as exc:
            raise ProviderError("llm_unreachable") from exc

        if response.stop_reason == "refusal":
            return LlmResult(text="", refused=True, model=response.model)
        text = "".join(b.text for b in response.content if b.type == "text").strip()
        return LlmResult(text=text, truncated=response.stop_reason == "max_tokens", model=response.model)
