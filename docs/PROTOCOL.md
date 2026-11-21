# Protocols

There are two interfaces. They are independent: the robot never needs the laptop to run.

1. **USB debug link** (laptop ↔ robot): development, testing, first-time provisioning.
2. **Gateway API** (robot ↔ cloud AI gateway, HTTPS): the standalone voice conversation.

---

## 1. USB debug link

- Both USB-C connectors work (native USB-Serial/JTAG and the USB-UART bridge); 115200 baud on the
  bridge.
- Newline-delimited JSON, UTF-8, one object per line, **max 512 bytes** per line (longer lines are
  discarded up to the next newline and reported with `line_too_long`).
- Optional `"v":1` (protocol version) and `"id"` (unsigned integer echoed in the ack).
- Every command gets exactly one `{"evt":"ack","id":N,"ok":true|false,...}`. On failure: `err`
  (code) and optionally `detail` (field name). Some acks include `"state"` (resulting state) and
  `"truncated":true` (a text field was cut at a UTF-8 boundary to fit).
- Non-JSON lines from the board (boot ROM messages, crash dumps) may appear; tools show them raw.

### Commands (laptop → robot)

| cmd | parameters | effect |
|---|---|---|
| `ping` | | ack only (round-trip test) |
| `hello` | | ack + `hello` event |
| `status` | | ack + `status` event |
| `state` | `state`: STARTUP/IDLE/LISTENING/PROCESSING (alias THINKING)/SPEAKING/LEARNING/MUTED/ERROR, `ms` deadline | validated transition (see rules below) |
| `face` | `expr`: idle, happy, listening, thinking, speaking, blink, sleeping, surprised, sad, muted, error; `ms` (default 2500) | temporary expression override (`blink` = one blink) |
| `text` | `text` (≤160 B), `rtl`, `ms` (default 4000) | full-screen text overlay |
| `show` | `kind`: letter/number/math/word, `value` (≤32 B), `caption` (≤160 B), `rtl`, `ms` (0 = until changed) | learning card, enters LEARNING |
| `say` | `text`, `rtl`, `ms` | SPEAKING face + subtitle ticker (visual only) |
| `mute` | `on`: bool | software microphone mute (stops the I2S peripheral) |
| `clear` | | remove overrides/content, back to rest state |
| `selftest` | | plays every expression and screen, emitting `selftest` events |
| `config` | `key`, `value` | store a setting in NVS (keys below) |
| `config_get` | | `config` event (secrets shown only as `*_set: true/false`) |
| `reboot` | | restart after 300 ms |
| `i2c_scan` | | `i2c_scan` event |
| `mic_test` | `ms` (≤10000, default 3000) | records and reports levels: `mic_test` event with `verdict` signal/all_zero/stuck/no_frames |
| `tone` | `hz` (50–8000), `ms` (≤5000) | sine tone through the speaker |
| `loopback` | `ms` (≤8000) | record then play back (ear test of mic + speaker) |
| `wifi_scan` | | `wifi_scan` event |
| `turn_text` | `text`, `lang` | sends text to the gateway as if spoken; shows/plays the reply |
| `read` | `text` (≤600 B), `rtl`, `ms` | shows text the way answers are shown without a speaker (SPEAKING + scrolling text) |

**Settings keys** (`config`): `wifi_ssid` (≤32), `wifi_pass` (8–63, secret), `gateway_url`
(`https://…`; `http://` only to private LAN IPs for development), `device_token` (16–128 chars
`[A-Za-z0-9._-]`, secret), `lang` (auto/en/ar), `child_age` (3–12), `volume` (0–100), `mic_gain`
(1–32), `oled_driver` (sh1106/ssd1306, reboot), `mic_channel` (left/right, reboot), `speaker`
(on/off, reboot; **off by default**: answers are shown as scrolling text and no audio is requested). Secret values
are never printed, logged or returned, and are never silently truncated (too long = rejected).

### State rules (enforced by the firmware, never by the AI)

- STARTUP cannot be requested and cannot be left early (except to ERROR); commands during it are
  rejected with `starting`.
- The microphone is **muted at power-on**; STARTUP ends in MUTED.
- LISTENING is rejected while muted (`mic_muted`). Muting during LISTENING drops the capture.
- IDLE means "rest": it resolves to MUTED while muted. MUTED cannot be requested; use `mute`.
- PROCESSING (25 s) and SPEAKING (90 s) have deadlines; recoverable ERRORs clear after 6 s.
  Deadlines are what recover the robot when the network or gateway stops answering.

### Events (robot → laptop)

`hello`, `status` (its `gateway` object includes `memory`: true while the robot holds a conversation
token), `ack`, `state` (`from`, `to`, `reason`: request/timeout/mute/unmute/startup_done/fault,
`error`), `mute`, `selftest` (`step`, `total`, `name` / `done`), `turn` (`stage`: speech_start,
submitted, reply, spoken, ignored, discarded, dropped, not_sent, failed, audio_missing,
audio_failed), `log`
(`lvl`, `msg`), `config`, `config_saved`, `i2c_scan`, `mic_test`, `wifi_scan`, `loopback`.

The diagnostic firmware prints `{"evt":"diag","stage":...}` lines instead (chip, flash, psram,
i2c_lines, i2c_scan, oled, summary); send `r` for a fresh report or `s` to rescan.

---

## 2. Gateway API (robot → gateway)

All requests: `Authorization: Bearer <device token>`, plus `X-Robot-Lang` (auto/en/ar),
`X-Robot-Age` (3–12), `X-Robot-Fw`, and `X-Robot-Output: text` from a robot without a speaker
(the gateway then skips text-to-speech, asks the model for ≤ 25 words, and adds
`display.long_text` / `display.long_rtl`: the whole answer, shaped for the OLED, ≤ 600 bytes).
HTTPS only in production. Every answered reply also carries `display.long_text` /
`display.long_rtl` (since gateway 0.4.0, also for speaker robots), so a robot whose speech cannot
be fetched can show the whole answer instead.

**Conversation memory (`ctx` / `X-Robot-Context`).** Every answered turn's reply carries `ctx`, an
opaque token (base64url, ≤ 2400 characters) sealed by the gateway with AES-256-GCM: the tutor
state and the last few exchanges, bound to the device, valid for 30 minutes. The robot keeps it in
RAM only and sends it back unchanged in the `X-Robot-Context` header of its next turn. This lets a
serverless gateway (Vercel), where consecutive requests can reach different instances, continue the
conversation without any server memory. Rules for the robot: `ctx` present → replace the stored
token; `ctx` absent (ignored turn, older gateway) → keep it; `ctx` empty or invalid (not base64url,
longer than 3000) → clear it. The gateway treats a missing, expired, foreign or altered token as
the start of a new conversation (never as an error). Robots with firmware < 0.3.0 send no token:
on a long-running host (Docker) the gateway still remembers them in memory; on Vercel each of their
turns starts fresh.

### `POST /v1/turns` — one spoken utterance

`Content-Type: audio/L16;rate=16000;channels=1`, body = raw PCM s16le mono, ≤ 15 s.

### `POST /v1/turns/text` — testing

`{"text": "...", "lang": "en"}` (≤ 500 characters).

### Reply (both)

```json
{
  "ignore": false,
  "lang": "en",
  "transcript": "what is seven plus five",
  "reply_text": "7 + 5 equals 12. Great question!",
  "display": {
    "oled_text": "7 + 5 = 12",
    "rtl": false,
    "content": {"kind": "math", "value": "7 + 5 = 12", "caption": ""}
  },
  "emotion": "happy",
  "audio": {"path": "/v1/turns/Xy.../audio", "format": "pcm_s16le", "sample_rate": 22050,
            "bytes": 177340, "duration_ms": 4021},
  "source": "math",
  "warnings": [],
  "timing": {"stt_ms": 0, "think_ms": 2, "tts_ms": 31, "total_ms": 33}
}
```

- `ignore: true` → nothing intelligible was said; the robot silently goes back to listening.
- Arabic `oled_text`, `value` and `caption` are **pre-shaped presentation forms in visual order**
  (`rtl: true`); the robot has no shaping engine. `oled_text` ≤ 150 bytes, `value` ≤ 32 bytes.
- `emotion` ∈ happy, sad, surprised, idle, thinking (the robot ignores anything else).
- The robot accepts `audio.path` only as a relative `/v1/...` path (≤ 1023 characters of
  `[A-Za-z0-9/_.-]`, no `..`) on the same gateway (so its token can never be sent elsewhere),
  `format` = `pcm_s16le`, `sample_rate` ∈ 8000/16000/22050/24000. `bytes` / `duration_ms` are
  present only when known (a serverless gateway synthesises speech when the audio is fetched).
  Replies over 12 KB are rejected. Bodies may use `Content-Length` or chunked transfer encoding.
- With the Groq Orpheus voice the rate is 24000 and the audio is streamed with chunked transfer
  encoding, one piece (≤ 200 characters of text) at a time.

### `GET /v1/turns/{id}/audio`

Reply speech, `audio/L16;rate=N;channels=1`. Only the device that made the turn can fetch it;
it expires after 120 s. On a long-running host the audio is stored once and deleted after the
first fetch. On a serverless host `{id}` is a sealed ticket (text, language, rate, expiry; bound
to the device) and the speech is synthesised during the GET, so a repeat fetch within 120 s is
possible (same device only). Audio is capped at 4 MB. Errors: 429 `{"error":"tts_rate_limited"}`
when the voice service's quota is used up, 500/502 for other provider failures, 404 for an unknown
or expired id. Firmware ≥ 0.4.0 then shows `long_text` instead (turn event `audio_failed`).

### `GET /healthz`

No authentication; never returns keys or secrets. `ok` (false + HTTP 503 when a provider could not
be configured, see `errors`), `version`, `stt`, `llm`, `tts`, `llm_model`, `tts_model`, `devices` (registered robots),
`stateless`, `memory` (`context` or `context+server`), `context_key` (`CONTEXT_SECRET` / `derived` /
`random` — where the sealing key comes from), `warnings` (configuration problems in plain words).

### Errors

JSON `{"error": "<code>"}`: 401 `unauthorized`, 413 `too_large`, 415 `expected_audio_l16` /
`unsupported_audio_format`, 429 `rate_limited` (the gateway's own limit) or `stt_rate_limited` /
`tts_rate_limited` (the provider's limit), 400 `audio_too_short` / `expected_json_text`,
502 provider failure (retryable), 500 provider misconfiguration. A rate-limited *language model*
is not an HTTP error: the reply is a kind "let's take a break" answer with
`"source":"fallback"` and the code in `warnings`. The robot maps 401/403 to a
sticky `auth_rejected` error (fix the token over USB), everything else to recoverable errors.
