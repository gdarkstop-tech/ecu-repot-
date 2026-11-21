"""Claude as the conversation model, via the official Anthropic Python SDK.

The API key is read by the SDK from ANTHROPIC_API_KEY (never logged or returned). It is an
Anthropic API (Console) key, billed per token — a Claude.ai subscription does not cover it.

Default: Claude Haiku 5.5 at effort "low" — the fast, low-cost model, suited to short replies
for a child. Thinking is adaptive (on by default on this model); effort is the speed/cost lever.
Server-side refusal fallbacks are sent only to models that support them (Haiku 5.5 does not:
there a refusal is handled by the gateway's safe fallback reply).
"""
from __future__ import annotations

import logging

import anthropic

from .base import ChatMessage, LlmResult, ProviderError

log = logging.getLogger("gateway.anthropic")

# Models that accept the server-side `fallbacks` parameter (beta server-side-fallback-2026-07-01).
FALLBACK_MODELS = {"claude-fable-5-2", "claude-fable-5-1", "claude-opus-5-5", "claude-opus-5", "claude-sonnet-5-5"}


class AnthropicLLM:
    name = "anthropic"

    def __init__(self, model: str = "claude-haiku-5-5", effort: str = "low", fallbacks: bool = True,
                 client: anthropic.Anthropic | None = None, max_tokens: int = 2048) -> None:
        # A short timeout: a child is waiting. The SDK retries 429/5xx/connection errors itself.
        self.client = client or anthropic.Anthropic(timeout=25.0, max_retries=1)
        self.model = model
        self.effort = effort
        self.fallbacks = fallbacks and model in FALLBACK_MODELS
        self.max_tokens = max_tokens  # leaves room for adaptive thinking plus a short answer

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
            billing = "credit balance" in str(getattr(exc, "message", "")).lower()
            code = "llm_billing_required" if billing else "llm_bad_request"
            log.warning("provider error: provider=anthropic what=llm model=%s status=400 -> %s", self.model, code)
            raise ProviderError(code, retryable=False) from exc
        except anthropic.RateLimitError as exc:
            raise ProviderError("llm_rate_limited") from exc
        except anthropic.APIStatusError as exc:
            raise ProviderError(f"llm_http_{exc.status_code}") from exc
        except anthropic.APIConnectionError as exc:
            raise ProviderError("llm_unreachable") from exc

        usage = getattr(response, "usage", None)
        in_tok = int(getattr(usage, "input_tokens", 0) or 0)
        out_tok = int(getattr(usage, "output_tokens", 0) or 0)
        if response.stop_reason == "refusal":
            return LlmResult(text="", refused=True, model=response.model, input_tokens=in_tok, output_tokens=out_tok)
        # Read blocks by type: the response can start with (empty) thinking blocks.
        text = "".join(b.text for b in response.content if b.type == "text").strip()
        return LlmResult(text=text, truncated=response.stop_reason == "max_tokens", model=response.model,
                         input_tokens=in_tok, output_tokens=out_tok)
