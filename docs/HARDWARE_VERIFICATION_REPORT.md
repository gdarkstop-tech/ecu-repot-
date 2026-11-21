# Hardware verification report — current status

**Date:** 2026-11-21 · **Firmware:** 0.4.0 · **Gateway:** 0.4.0

> **The physical ESP32-S3 has not been flashed or tested yet.** This work was done in a cloud
> development container that has no access to the laptop's USB ports (no Remote Control folder was
> available). Everything below is marked by how it was verified. The hardware checks run with one
> command on the laptop — `python tools/verify_hardware.py` — which writes a dated report from
> what it observes, in `reports/`.

Legend: ✅ verified · 🟡 verified without the physical robot (build, unit test, simulator, local server) · ⬜ not tested yet

## Requested report items

| Item | Status | Evidence / next step |
|---|---|---|
| Detected board | ⬜ | Run `verify_hardware.py` (uses esptool `flash_id` + diag firmware) |
| COM port | ⬜ | Auto-detected by USB VID:PID (303A native, CP210x, CH34x) on the laptop |
| Flash / PSRAM configuration | 🟡 | Build configured for 16 MB QIO flash + 8 MB OPI PSRAM (`qio_opi`); image header reads 16 MB. Runtime size + PSRAM read/write test run on the board via the diag firmware |
| Build result | ✅ | `diag`: SUCCESS, 318 KB (4.9% of 6.25 MB app partition). `robot`: SUCCESS, 1.09 MB (16.7%), 80 KB static RAM (24.6%). No warnings in project code |
| Flash result | ⬜ | Not possible from the cloud container |
| OLED detection | ⬜ | Diag firmware scans I2C (GPIO8/9), checks pull-ups, inits SH1106, asks you to confirm the test pattern |
| Working animations | 🟡 | All 11 expressions + learning cards + Arabic text rendered through the real U8g2 library on the host: `docs/media/oled_gallery.png` and GIFs. Not yet seen on the physical OLED |
| Laptop communication | 🟡 | 15/15 protocol checks pass against the host simulator (same `robot_logic` code): ping, malformed/unknown/over-long input, mute rules, transitions, deadline recovery, selftest |
| AI mode tested | 🟡 | Gateway in keyless mode (mock STT + deterministic tutor + eSpeak NG voice) ran under uvicorn and answered real HTTP requests from the robot-imitating client. Claude provider tested against a mock HTTP transport (request shape, refusal, errors) — no API key used. OpenAI-compatible providers not exercised |
| Features not yet tested | ⬜ | Everything on physical hardware; the full list follows |
| Missing physical connections | — | Per your note, only the OLED is assumed connected. Mic, speaker, button, camera, sensors and motors are not assumed |

## Verified in this session (🟡/✅)

| Area | Result |
|---|---|
| Firmware unit tests (host) | **93/93 pass** (`pio test -e native`), incl. the firmware↔gateway contract (14), the HTTP body decoder (7, one feeding the exact chunked bytes a real uvicorn server streamed for a two-piece Orpheus answer), text-only reading mode, and the speech-failure → text fallback (3) |
| Groq (mocked API) | **28 tests** with a mocked Groq API (httpx MockTransport): gpt-oss request shape (`include_reasoning:false`, `reasoning_effort:low`), English/Arabic open questions reaching the model with memory, tutor/safety answering without calling Groq, 429 retry policy, error mapping, safe logs (no key/text/org id), Orpheus pieces ≤ 200 chars, WAV formats → 24 kHz mono s16 with −1 dBFS peak, streamed audio from another instance, 429 on the audio fetch. **Not live**: no Groq key was used in this session |
| Groq endpoint reachability | Real `api.groq.com` chat and speech endpoints called with a deliberately fake key: both answered `401 invalid_api_key`, mapped to `llm_auth_failed` / `tts_auth_failed`; the fake key never appeared in logs. Proves URLs and error format only — not answers or audio |
| Gateway tests | **109/109 pass** (81 earlier + 28 Groq): auth, size/format limits, rate limit, device-bound audio, math engine (EN/AR words, Arabic-Indic digits), tutor modes/difficulty/quiz, safety filters, LLM output cleaning + arithmetic correction, Claude request shape, Groq/OpenAI STT adapter, text-only mode, eSpeak EN/AR, and 14 serverless tests (below) |
| Contract | The firmware's own C++ parser reads 15 real gateway replies (EN/AR math, alphabet, story, vocab, ignore, demo, text-only, and Vercel-mode replies with conversation tokens and a sealed audio ticket) and rejects hostile ones (foreign audio URL, path traversal, bad format/rate, unknown emotion, invalid UTF-8, header-injecting or oversized context tokens) |
| Vercel compatibility | Vercel CLI 65.16.0 `vercel build` of `gateway/` (local, no account): detects `index.py`, runtime `python3.12`, bundle ≈ 40 MB (limit 500 MB); with preset "Other" no function is built, so `vercel.json` pins `fastapi`. All dependencies have Linux x86_64 wheels for Python 3.12/3.13/3.14. Clean Python 3.12 install: 90 MB, entrypoint import 0.3 s. Two separate gateway processes sharing only `CONTEXT_SECRET`: a math lesson continued correctly while alternating between them; audio made on one was fetched from the other; tampered/foreign/expired/other-key tokens start fresh; the entrypoint writes no files (audit hook). **Not done: a live Vercel deployment and real Claude/Groq calls** (no account or keys here; vercel.com is not reachable from this session) |
| Gateway over real HTTP | Arabic `6 × 7` → `42` with shaped subtitle + 4.9 s of speech audio; demo audio turn honest; wrong token → 401; device token absent from server log |
| Docker image | Dockerfile written; **build not verified** (this sandbox's build containers have no DNS) |

## Hardware checklist (run on the laptop)

Each line is produced by `tools/verify_hardware.py` unless marked *manual*.

**Phase 1 — board + OLED**
- [ ] USB port detected, esptool reads chip = ESP32-S3, flash 16 MB, PSRAM 8 MB
- [ ] Diag firmware: serial output, flash JEDEC 16 MB, PSRAM test pass, I2C finds 0x3C/0x3D, SH1106 init
- [ ] Diag test pattern visible with no edge shift (operator)
- [ ] Robot firmware: hello, ping RTT, muted at startup, protocol robustness, transitions, recovery
- [ ] Expressions animate smoothly; Arabic glyph visible (operator)

**Phase 2 — audio (`--audio`, after wiring)**
- [ ] INMP441 returns real samples; speech ≥ 6 dB above silence
- [ ] Speaker tone heard (operator); mic→speaker loopback heard (operator)

**Phase 3 — network + AI**
- [ ] Vercel deployment: `/healthz` shows `"ok":true`, `"version":"0.4.0"`, `"llm":"groq"`, `"llm_model":"openai/gpt-oss-120b"`, `"devices":1`, `"context_key":"CONTEXT_SECRET"` (*manual*, docs/VERCEL_SETUP.md Part G)
- [ ] Laptop client against Vercel: "Why do cats purr?" → `"source":"llm"` (Groq), Arabic question answered in Arabic, two-turn math remembered (*manual*, Part D)
- [ ] Voice: `gateway_client.py` without `--no-speaker` saves `reply.wav` at 24 kHz that sounds right; robot with `speaker on` speaks the answer at `volume 70`; after the daily voice quota the robot shows the answer as text (*manual*)
- [ ] Wi-Fi connects (2.4 GHz); gateway text turn produces reply → spoken or scrolled on the OLED; `status` shows `"memory":true` after the first answer

**Phase 4 — standalone (manual, laptop disconnected)** — see README section 7
- [ ] Power bank only: boot, unmute, ask a question by voice, hear the answer, robot listens again
- [ ] Arabic voice conversation
- [ ] Robot does not answer its own voice
- [ ] Wi-Fi off → offline cards; Wi-Fi back → conversations resume

**The voice AI requirement is not complete until the Phase 4 items pass on the physical robot.**
