# Architecture

## Goal

A child talks to the robot; the robot answers out loud, in Arabic or English, with an animated
face. In normal use **no laptop is involved**: the robot runs on a power bank and talks to an
independently hosted AI gateway over Wi-Fi.

```
            ┌──────────────────────── ESP32-S3 N16R8 (the robot) ────────────────────────┐
 child ───▶ │ INMP441 ─I2S0─▶ mic task ─▶ VAD / end-of-speech ─▶ utterance buffer (PSRAM)    │
 speech     │                                   │                       │                   │
            │                       conversation loop ◀── state machine ─┤                   │
            │                                   │                       │  OLED face 30 fps │
 robot  ◀── │ MAX98357A ◀─I2S1─ speaker task ◀──┤ stream buffer (PSRAM) └─▶ SH1106 (I2C)    │
 voice      │                                   │                                           │
            │      mute button (GPIO4) ─────────┘   offline learning cards (no network)      │
            │      USB debug link (development / provisioning only)                          │
            └──────────────────────────────────────│────────────────────────────────────────┘
                                                   │ HTTPS (device token, pinned root CAs)
                                    ┌──────────────▼──────────────── AI gateway (cloud) ─────┐
                                    │ auth ─▶ rate limit ─▶ STT ─▶ child-safety ─▶ tutor     │
                                    │   (math computed, modes, quizzes) ─▶ LLM (Claude, …)   │
                                    │   ─▶ safety + arithmetic check ─▶ OLED text (shaped)   │
                                    │   ─▶ TTS ─▶ one-time reply audio                       │
                                    │ provider keys live ONLY here                           │
                                    └────────────────────────────────────────────────────────┘
```

## Components

| Path | What | Verified how |
|---|---|---|
| `firmware/lib/robot_logic` | Portable C++: state machine, USB protocol, VAD, utterance capture, conversation loop, offline tutor, gateway-reply parser | 73 host unit tests (`pio test -e native`) |
| `firmware/lib/robot_ui` | Face/screens renderer on the U8g2 C API | Rendered on the host through the real U8g2 library (`docs/media`) |
| `firmware/lib/robot_hw` | ESP32 adapters: OLED, button, I2S mic/speaker, Wi-Fi, NVS settings, HTTPS gateway client | Compiles; **needs hardware verification** |
| `firmware/src/diag` | Minimal diagnostic firmware | Compiles; **needs hardware verification** |
| `firmware/sim` | Host simulator (same logic + renderer) | Used by tests and tools |
| `gateway/` | Python FastAPI AI gateway | 55 tests; ran under uvicorn with real HTTP |
| `tools/` | Laptop dev tools: verify/flash, console/provisioning, gateway client, generators | Verification tool exercised against the simulator |

## Key decisions (ADRs)

**ADR-1 — Hybrid: the robot does real-time work, the cloud does AI.** The ESP32-S3 cannot run a
capable language model or good Arabic STT/TTS. It owns everything time-critical or physical
(face, states, audio capture/playback, VAD, echo guard, offline mode); the gateway owns AI.

**ADR-2 — A gateway we host, not provider SDKs on the robot.** Cloud API keys never touch the
firmware (a robot can be lost or opened). Each robot holds only a revocable device token for our
gateway; the gateway holds provider keys, enforces rate limits and safety, and can switch providers
(Claude, OpenAI-compatible, keyless demo) by configuration without reflashing.

**ADR-3 — Request/response turns, not streaming audio both ways.** One utterance = one HTTPS POST
of 16 kHz PCM; the reply metadata returns first (subtitle + face immediately), then reply audio
streams into a PSRAM buffer with backpressure. Simple, debuggable, robust to Wi-Fi hiccups;
latency is acceptable for a child's turn-taking. Streaming STT/TTS is a later optimisation.

**ADR-4 — Energy VAD with an adaptive noise floor.** Deterministic, cheap (microseconds per 20 ms
frame), unit-tested with synthetic signals, needs no model download. Pre-roll keeps the first
syllable. A wake word is a future option (hands-free + always listening when unmuted is the
specified behaviour; the mute button is the control).

**ADR-5 — Echo prevention by design (half-duplex).** The microphone peripheral is stopped while
the robot is processing/speaking and for 400 ms after playback drains; the VAD speech state is
reset before re-arming; the amplifier is shut down (SD pin) when idle. No acoustic echo
cancellation is needed for turn-taking; barge-in (interrupting the robot) is not supported.

**ADR-6 — The AI never controls the robot.** Model output is data: the gateway cleans it, checks
it for child safety, verifies arithmetic, shapes it for the display, and returns a fixed schema.
The firmware validates every field (allow-listed emotions and content kinds, bounded UTF-8,
same-origin relative audio path). Children's words reach the model inside a delimited field.

**ADR-7 — Arabic shaping on the gateway.** U8g2 has a 16 px Arabic font but no shaping/bidi.
The gateway (and the offline-content generator) send presentation forms in visual order; the
renderer wraps RTL lines from the right and scales single glyphs up by ink box.

**ADR-8 — Deterministic tutor around the model.** Learning modes, quizzes, answer checking,
difficulty-by-age and all arithmetic are plain code; the model only handles open questions. This
keeps answers correct and the robot useful even with no model configured (demo mode).

**ADR-9 — One firmware, both USB ports.** The debug link reads/writes the native USB and the
USB-UART bridge simultaneously, so it works whichever connector is used. The link is optional.

**ADR-10 — Single-process gateway state (long-running hosts).** On Docker/a VPS, sessions, rate
limits and one-time reply audio are in memory; run one worker. Kept for robots with older firmware.

**ADR-11 — Stateless gateway: the robot carries its sealed conversation (serverless hosts).** Vercel
(free Hobby plan) runs each request on any instance and keeps no memory or files, and a shared
database would add a service to set up and run. Instead, the gateway seals the conversation state
(tutor mode, pending question, level, quiz score, last 4 exchanges) into a token with AES-256-GCM
(key from `CONTEXT_SECRET`, associated data = purpose + device id, issued-at for a 30-minute expiry,
zlib-compressed, ≤ 2400 characters, oldest exchanges dropped first) and returns it as `ctx`; the
robot stores it in RAM and returns it as `X-Robot-Context`. Integrity matters most: a forged
history would be a prompt-injection path into the model, so any token that fails authentication,
expiry or schema validation silently starts a fresh session. Encryption keeps the child's words
private even if the token is logged somewhere. Reply audio uses the same sealing for a ticket in the
URL (speech synthesised on the GET). Trade-offs: tokens cannot be revoked individually (rotate
`CONTEXT_SECRET` to invalidate all); audio tickets are replayable by the same device for 120 s;
rate limits become per-instance (best effort — provider spend limits are the hard cap); a robot
reboot starts a new conversation. Considered and rejected for now: Vercel-Marketplace Redis/KV (an
extra account and setup for a beginner, free tiers vary) and returning audio inline in the JSON
(the robot's 12 KB reply buffer).

**ADR-12 — Groq as the default AI provider; natural voice from Groq Orpheus.** Anthropic API
use needs prepaid credits, which stopped real conversations. Groq offers a free plan and one key
covers Whisper (already used), the language model and speech, so the default is now
`openai/gpt-oss-120b` (configurable `GROQ_LLM_MODEL`; `reasoning_effort=low`,
`include_reasoning=false` so only the answer returns) and, optionally, Orpheus for the voice.
Claude stays a configuration choice (`LLM_PROVIDER=anthropic`). Orpheus accepts ≤ 200 characters
per request and the free plan allows ~100 speech requests a day, so the gateway splits speech at
sentence boundaries, speaks at most `GROQ_TTS_MAX_CHUNKS` (2) pieces per answer, converts the
WAV to 24 kHz s16le mono (resampling only on a mismatch), limits peaks to −1 dBFS, and streams
piece by piece (first-piece errors become the HTTP status, later ones end the audio early).
Free-plan limits are respected, not fought: at most one retry when Groq asks for a ≤ 2 s pause;
otherwise a rate-limited model gives a kind "break" answer, a rate-limited voice returns 429 and
firmware ≥ 0.4.0 shows the whole answer as text. The gateway never upgrades a plan or switches to
a paid provider by itself. Provider failures are logged with provider, model, status and the
provider's error type/code only (no key, no child text, no provider free-text message).

**ADR-13 — A display-independent face model, painted in BMO style.** The robot is dressed as
BMO, so the OLED is BMO's screen: dark dot eyes and a simple mouth on a lit screen (the mono
buffer is inverted at the end of each frame; sleeping switches to a dark screen). Expressions are
data — `FacePose` (eye kinds, mouth kind, marks, offsets) produced by a deterministic
`FaceAnimator` (seeded blink schedule, fixed small gaze pattern, eased posture, quantised talking
mouth, expression changes behind a blink) — in `robot_logic`, unit-tested on the host. The
U8g2 painter maps semantic colour roles (Screen / Feature / Accent) to lit, dark and dithered
pixels, so a colour display later needs only a new painter. With a speaker the face does the
talking: no subtitles or long text (Arabic presentation forms render poorly at 16 px); a failed
voice shows a worried face and a short English label. Drawings are original geometry, not
copied artwork.

## Security and privacy

- **No cloud keys on the robot.** Device token stored in NVS; write-only over USB; never printed.
- **TLS** to the gateway, validated against public root CAs generated from Mozilla's bundle
  (`tools/gen_ca_roots.py`). Plain HTTP is refused except to private LAN addresses (development).
- **Revocation:** `python -m robot_gateway.devices disable robot-1`; the gateway stores only
  SHA-256 hashes of tokens.
- **Children's data:** microphone audio is never stored by the gateway; transcripts are not logged
  unless `LOG_TRANSCRIPTS=true`; conversation memory is an encrypted token held in the robot's RAM
  (and, on long-running hosts, in the gateway's RAM) and expires after 30 minutes idle; reply audio
  is deleted after one fetch or 120 s (serverless: never stored, ticket expires after 120 s). Phone numbers/emails/addresses are caught before
  any model call. Choose AI providers whose data terms fit children's data in your jurisdiction.
- **Limits:** 15 s max utterance, 20 turns/min and 1,500/day per robot (configurable; per instance
  on serverless hosts). Set a monthly spend limit in the Anthropic Console as the hard cost cap.
- **Robot-side parsing:** reply bodies are read with a 12 KB cap and a 10 s stall timeout whatever
  the HTTP framing (Content-Length or chunked); the context token is accepted only as base64url
  (no header injection) and never partially written.
- **Known gaps (production hardening):** anyone with physical USB access can reprovision or
  reflash the robot — enable ESP32-S3 secure boot + flash encryption and disable the debug console
  for production units. The keyword safety lists are a baseline; add a provider moderation API and
  human review of content for a commercial product.

## What is not built yet

Camera/object recognition (ESP32-CAM), distance sensor, motors/servo, microSD content, wake word,
barge-in, streaming STT/TTS, exact cross-instance rate limiting, OTA firmware updates, parent
dashboard.
