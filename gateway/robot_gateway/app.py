"""HTTP API used by the robot.

  POST /v1/turns              body: audio/L16;rate=16000;channels=1   (the child's utterance)
  POST /v1/turns/text         body: {"text": "...", "lang": "en|ar|auto"}   (testing)
  GET  /v1/turns/{id}/audio   reply speech, PCM s16le mono, one-time, same device only
  GET  /healthz               liveness + which providers are configured (no secrets)

Every /v1 call needs "Authorization: Bearer <device token>". Run behind HTTPS (the hosting
platform's TLS termination); the robot refuses plain HTTP except to private LAN addresses.
"""
from __future__ import annotations

import json
import logging
import re

from fastapi import Depends, FastAPI, Header, HTTPException, Request
from fastapi.responses import JSONResponse, Response
from starlette.concurrency import run_in_threadpool

from . import __version__
from .config import Settings, load_dotenv
from .devices import Device, DeviceRegistry
from .pipeline import TurnPipeline
from .providers.base import LanguageModel, ProviderError, SpeechToText, TextToSpeech
from .providers.factory import build_llm, build_stt, build_tts
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


def create_app(settings: Settings | None = None, *, stt: SpeechToText | None = None,
               llm: LanguageModel | None = None, tts: TextToSpeech | None = None,
               registry: DeviceRegistry | None = None) -> FastAPI:
    if settings is None:
        load_dotenv()
        settings = Settings.from_env()
    stt = stt or build_stt(settings)
    llm = llm or build_llm(settings)
    tts = tts or build_tts(settings)
    registry = registry or DeviceRegistry(settings.devices_file, settings.device_token_hashes)
    if len(registry) == 0:
        log.warning("no devices registered: every request will be rejected (see robot_gateway.devices)")

    limiter = RateLimiter(settings.turns_per_minute, settings.turns_per_day)
    sessions = SessionStore(settings.session_ttl_seconds)
    audio = AudioStore(settings.audio_ttl_seconds)
    pipeline = TurnPipeline(settings, stt, llm, tts, sessions, audio)

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
            raise HTTPException(502 if exc.retryable else 500, {"error": exc.code}) from exc

    def finish(dev: Device, result) -> JSONResponse:
        log.info("turn device=%s %s", dev.id, json.dumps(result.log_fields, ensure_ascii=False))
        return JSONResponse(result.body)

    @app.get("/")
    def root() -> dict:
        return {"service": "robot-ai-gateway", "version": __version__}

    @app.get("/healthz")
    def health() -> dict:
        return {"ok": True, "version": __version__, "stt": stt.name, "llm": llm.name, "tts": tts.name,
                "devices": len(registry)}

    @app.post("/v1/turns")
    async def turn_audio(request: Request, dev: Device = Depends(device_auth),
                         x_robot_lang: str | None = Header(default=None),
                         x_robot_age: str | None = Header(default=None),
                         x_robot_output: str | None = Header(default=None),
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
        result = await run_in_threadpool(run_or_502, pipeline.run_audio, dev.id, pcm, rate, lang, age, debug, output)
        return finish(dev, result)

    @app.post("/v1/turns/text")
    async def turn_text(request: Request, dev: Device = Depends(device_auth),
                        x_robot_lang: str | None = Header(default=None),
                        x_robot_age: str | None = Header(default=None),
                        x_robot_output: str | None = Header(default=None)) -> JSONResponse:
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
        result = await run_in_threadpool(run_or_502, pipeline.run_text, dev.id, text, lang, age, output)
        return finish(dev, result)

    @app.get("/v1/turns/{turn_id}/audio")
    def turn_audio_fetch(turn_id: str, dev: Device = Depends(device_auth)) -> Response:
        if not re.fullmatch(r"[A-Za-z0-9_-]{8,64}", turn_id):
            raise HTTPException(404, {"error": "not_found"})
        item = audio.take(turn_id, dev.id)
        if item is None:
            raise HTTPException(404, {"error": "not_found"})
        return Response(item.pcm, media_type=f"audio/L16;rate={item.rate};channels=1",
                        headers={"Cache-Control": "no-store"})

    return app


def app_factory() -> FastAPI:  # uvicorn --factory robot_gateway.app:app_factory
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")
    return create_app()
