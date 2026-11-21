"""HTTP API used by the robot.

  POST /v1/turns              body: audio/L16;rate=16000;channels=1   (the child's utterance)
  POST /v1/turns/text         body: {"text": "...", "lang": "en|ar|auto"}   (testing)
  GET  /v1/turns/{id}/audio   reply speech, PCM s16le mono, same device only, expires
  GET  /healthz               liveness + configuration status (never secrets)

Every /v1 call needs "Authorization: Bearer <device token>". Run behind HTTPS (the hosting
platform's TLS termination); the robot refuses plain HTTP except to private LAN addresses.

Conversation memory: replies carry "ctx" (a sealed token); the robot sends it back in the
X-Robot-Context header. That makes the gateway work on serverless hosts such as Vercel, where
consecutive requests may reach different instances (memory.py, docs/ARCHITECTURE.md ADR-11).
"""
from __future__ import annotations

import json
import logging
import os

from fastapi import Depends, FastAPI, Header, HTTPException, Request
from fastapi.responses import JSONResponse, Response, StreamingResponse
from starlette.concurrency import run_in_threadpool

from . import __version__
from .config import Settings, load_dotenv
from .devices import Device, DeviceRegistry
from .memory import ContextCodec, ConversationMemory
from .pipeline import TurnPipeline
from .providers.base import LanguageModel, ProviderError, SpeechToText, TextToSpeech
from .providers.factory import build_llm, build_stt, build_tts
from .sealing import Sealer, resolve_context_secret
from .speech import StoredSpeech, TicketSpeech, valid_turn_id
from .state import AudioStore, RateLimiter, SessionStore

log = logging.getLogger("gateway")


def _parse_l16(content_type: str | None) -> int:
    """Validate 'audio/L16;rate=16000;channels=1' and return the sample rate."""
    if not content_type or not content_type.lower().startswith("audio/l16"):
        raise HTTPException(415, {"error": "expected_audio_l16"})
    params = dict(p.strip().lower().split("=", 1) for p in content_type.split(";")[1:] if "=" in p)
    try:
        rate = int(params.get("rate", "16000"))
        channels = int(params.get("channels", "1"))
    except ValueError:
        raise HTTPException(415, {"error": "bad_audio_params"}) from None
    if channels != 1 or not 8000 <= rate <= 48000:
        raise HTTPException(415, {"error": "unsupported_audio_format"})
    return rate


async def _read_limited(request: Request, limit: int) -> bytes:
    declared = request.headers.get("content-length")
    if declared and declared.isdigit() and int(declared) > limit:
        raise HTTPException(413, {"error": "too_large"})
    body = bytearray()
    async for chunk in request.stream():
        body += chunk
        if len(body) > limit:
            raise HTTPException(413, {"error": "too_large"})
    return bytes(body)


class _Unavailable:
    """Stands in for a provider that could not be configured, so the gateway still starts and
    /healthz can say what is wrong (on Vercel a crash at import would only show a generic error)."""

    rate = 16000

    def __init__(self, kind: str, code: str) -> None:
        self.name, self.code = f"{kind}_unavailable", code

    def _fail(self, *_args, **_kwargs):
        raise ProviderError(self.code, retryable=False)

    transcribe = reply = synthesize = _fail


def _build(kind: str, factory, settings: Settings, errors: list[str]):
    try:
        return factory(settings)
    except ProviderError as exc:
        errors.append(f"{kind}: {exc.code}")
        log.error("%s provider not configured: %s", kind, exc.code)
        return _Unavailable(kind, exc.code)


def _config_warnings(settings: Settings, devices: int) -> list[str]:
    warnings = []
    if devices == 0:
        warnings.append("no robots registered: set DEVICE_TOKEN_HASHES")
    if settings.llm_provider == "anthropic" and not os.environ.get("ANTHROPIC_API_KEY"):
        warnings.append("ANTHROPIC_API_KEY is not set")
    if settings.llm_provider == "mock":
        warnings.append("LLM_PROVIDER is mock: demo answers only")
    if settings.stt_provider == "mock":
        warnings.append("STT_PROVIDER is mock: spoken words are not understood")
    return warnings


def create_app(settings: Settings | None = None, *, stt: SpeechToText | None = None,
               llm: LanguageModel | None = None, tts: TextToSpeech | None = None,
               registry: DeviceRegistry | None = None, stateless: bool | None = None,
               context_secret: bytes | None = None) -> FastAPI:
    if settings is None:
        load_dotenv()
        settings = Settings.from_env()
    stateless = settings.stateless if stateless is None else stateless
    errors: list[str] = []
    stt = stt or _build("stt", build_stt, settings, errors)
    llm = llm or _build("llm", build_llm, settings, errors)
    tts = tts or _build("tts", build_tts, settings, errors)
    registry = registry or DeviceRegistry(settings.devices_file, settings.device_token_hashes)
    if len(registry) == 0:
        log.warning("no devices registered: every request will be rejected (see robot_gateway.devices)")

    if context_secret is None:
        context_secret, key_source, key_warning = resolve_context_secret()
    else:
        key_source, key_warning = "provided", None
    warnings = _config_warnings(settings, len(registry)) + ([key_warning] if key_warning else [])
    for w in warnings:
        log.warning("config: %s", w)

    # Rate limits are per instance: exact on one long-running server, best effort on serverless
    # hosts (each instance counts separately). Provider-side spend limits are the hard cap.
    limiter = RateLimiter(settings.turns_per_minute, settings.turns_per_day)
    codec = ContextCodec(Sealer(context_secret, "ctx"), settings.session_ttl_seconds,
                         settings.context_max_chars, settings.context_history_turns)
    memory = ConversationMemory(codec, None if stateless else SessionStore(settings.session_ttl_seconds))
    speech = (TicketSpeech(tts, Sealer(context_secret, "audio"), settings.audio_ttl_seconds) if stateless
              else StoredSpeech(tts, AudioStore(settings.audio_ttl_seconds)))
    pipeline = TurnPipeline(settings, stt, llm, speech)

    app = FastAPI(title="Robot AI Gateway", version=__version__, docs_url=None, redoc_url=None, openapi_url=None)

    @app.exception_handler(HTTPException)
    async def http_error(_: Request, exc: HTTPException) -> JSONResponse:
        detail = exc.detail if isinstance(exc.detail, dict) else {"error": str(exc.detail)}
        return JSONResponse(detail, status_code=exc.status_code)

    def device_auth(authorization: str | None = Header(default=None)) -> Device:
        dev = registry.authenticate(authorization)
        if dev is None:
            raise HTTPException(401, {"error": "unauthorized"})
        return dev

    def robot_prefs(x_robot_lang: str | None, x_robot_age: str | None) -> tuple[str, int]:
        lang = (x_robot_lang or "auto").lower()
        lang = lang if lang in ("en", "ar") else "auto"
        age = int(x_robot_age) if x_robot_age and x_robot_age.isdigit() else 6
        return lang, age

    def run_or_502(fn, *args):
        try:
            return fn(*args)
        except ProviderError as exc:
            # 429 for a provider's rate limit (the robot shows a short "busy" error and recovers),
            # 502 for other retryable failures, 500 for configuration problems.
            status = 429 if exc.code.endswith("_rate_limited") else (502 if exc.retryable else 500)
            log.warning("turn failed: status=%d code=%s", status, exc.code)
            raise HTTPException(status, {"error": exc.code}) from exc

    def finish(dev: Device, session, mem: str, result) -> JSONResponse:
        if not result.body.get("ignore"):
            # Nothing changed on an ignored turn: no "ctx", so the robot keeps the one it has.
            result.body["ctx"] = memory.token_for(session)
        log.info("turn device=%s mem=%s %s", dev.id, mem, json.dumps(result.log_fields, ensure_ascii=False))
        return JSONResponse(result.body)

    @app.get("/")
    def root() -> dict:
        return {"service": "robot-ai-gateway", "version": __version__}

    @app.get("/healthz")
    def health() -> JSONResponse:
        body = {"ok": not errors, "version": __version__, "stt": stt.name, "llm": llm.name, "tts": tts.name,
                "llm_model": getattr(llm, "model", None), "tts_model": getattr(tts, "model", None),
                "devices": len(registry), "stateless": stateless, "memory": memory.mode,
                "context_key": key_source, "warnings": warnings, "errors": errors}
        return JSONResponse(body, status_code=200 if not errors else 503)

    @app.post("/v1/turns")
    async def turn_audio(request: Request, dev: Device = Depends(device_auth),
                         x_robot_lang: str | None = Header(default=None),
                         x_robot_age: str | None = Header(default=None),
                         x_robot_output: str | None = Header(default=None),
                         x_robot_context: str | None = Header(default=None),
                         x_debug_transcript: str | None = Header(default=None)) -> JSONResponse:
        rate = _parse_l16(request.headers.get("content-type"))
        if not limiter.allow(dev.id):
            raise HTTPException(429, {"error": "rate_limited"})
        limit = int(settings.max_audio_seconds * rate * 2) + 64
        pcm = await _read_limited(request, limit)
        if len(pcm) < rate // 10 * 2:  # < 100 ms
            raise HTTPException(400, {"error": "audio_too_short"})
        lang, age = robot_prefs(x_robot_lang, x_robot_age)
        debug = x_debug_transcript if settings.allow_debug_transcript else None
        output = "text" if (x_robot_output or "").lower() == "text" else "audio"
        session, mem = memory.load(dev.id, x_robot_context)
        result = await run_in_threadpool(run_or_502, pipeline.run_audio, session, pcm, rate, lang, age, debug, output)
        return finish(dev, session, mem, result)

    @app.post("/v1/turns/text")
    async def turn_text(request: Request, dev: Device = Depends(device_auth),
                        x_robot_lang: str | None = Header(default=None),
                        x_robot_age: str | None = Header(default=None),
                        x_robot_output: str | None = Header(default=None),
                        x_robot_context: str | None = Header(default=None)) -> JSONResponse:
        if not limiter.allow(dev.id):
            raise HTTPException(429, {"error": "rate_limited"})
        raw = await _read_limited(request, 8192)
        try:
            data = json.loads(raw)
            text = data["text"]
            assert isinstance(text, str)
        except (ValueError, KeyError, AssertionError, TypeError):
            raise HTTPException(400, {"error": "expected_json_text"}) from None
        if not text.strip() or len(text) > settings.max_text_chars:
            raise HTTPException(400, {"error": "bad_text_length"})
        lang, age = robot_prefs(data.get("lang") or x_robot_lang, x_robot_age)
        output = "text" if (x_robot_output or data.get("output") or "").lower() == "text" else "audio"
        session, mem = memory.load(dev.id, x_robot_context)
        result = await run_in_threadpool(run_or_502, pipeline.run_text, session, text, lang, age, output)
        return finish(dev, session, mem, result)

    @app.get("/v1/turns/{turn_id}/audio")
    def turn_audio_fetch(turn_id: str, dev: Device = Depends(device_auth)) -> Response:
        if not valid_turn_id(turn_id):
            raise HTTPException(404, {"error": "not_found"})
        item = run_or_502(speech.fetch, turn_id, dev.id)
        if item is None:
            raise HTTPException(404, {"error": "not_found"})
        pcm, rate = item
        media = f"audio/L16;rate={rate};channels=1"
        if isinstance(pcm, (bytes, bytearray)):
            return Response(pcm, media_type=media, headers={"Cache-Control": "no-store"})
        # Natural voice in pieces: stream each one as it is synthesised (chunked transfer encoding).
        return StreamingResponse(pcm, media_type=media, headers={"Cache-Control": "no-store"})

    return app


def app_factory() -> FastAPI:  # uvicorn --factory robot_gateway.app:app_factory
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")
    return create_app()
