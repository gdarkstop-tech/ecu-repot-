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
| `turn_text` | `text`, `lang` | sends text to the gateway as if spoken; plays the reply |

**Settings keys** (`config`): `wifi_ssid` (≤32), `wifi_pass` (8–63, secret), `gateway_url`
(`https://…`; `http://` only to private LAN IPs for development), `device_token` (16–128 chars
`[A-Za-z0-9._-]`, secret), `lang` (auto/en/ar), `child_age` (3–12), `volume` (0–100), `mic_gain`
(1–32), `oled_driver` (sh1106/ssd1306, reboot), `mic_channel` (left/right, reboot). Secret values
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

`hello`, `status`, `ack`, `state` (`from`, `to`, `reason`: request/timeout/mute/unmute/startup_done/fault,
`error`), `mute`, `selftest` (`step`, `total`, `name` / `done`), `turn` (`stage`: speech_start,
submitted, reply, spoken, ignored, discarded, dropped, not_sent, failed, audio_missing), `log`
(`lvl`, `msg`), `config`, `config_saved`, `i2c_scan`, `mic_test`, `wifi_scan`, `loopback`.

The diagnostic firmware prints `{"evt":"diag","stage":...}` lines instead (chip, flash, psram,
i2c_lines, i2c_scan, oled, summary); send `r` for a fresh report or `s` to rescan.

---

## 2. Gateway API (robot → gateway)

All requests: `Authorization: Bearer <device token>`, plus `X-Robot-Lang` (auto/en/ar),
`X-Robot-Age` (3–12), `X-Robot-Fw`. HTTPS only in production.

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
- The robot accepts `audio.path` only as a relative `/v1/...` path on the same gateway
  (so its token can never be sent elsewhere), `format` = `pcm_s16le`, `sample_rate` ∈
  8000/16000/22050/24000. Replies over 8 KB are rejected.

### `GET /v1/turns/{id}/audio`

Reply speech, `audio/L16;rate=N;channels=1` with `Content-Length`. One-time fetch, only by the
device that made the turn, expires after 120 s.

### Errors

JSON `{"error": "<code>"}`: 401 `unauthorized`, 413 `too_large`, 415 `expected_audio_l16` /
`unsupported_audio_format`, 429 `rate_limited`, 400 `audio_too_short` / `expected_json_text`,
502 provider failure (retryable), 500 provider misconfiguration. The robot maps 401/403 to a
sticky `auth_rejected` error (fix the token over USB), everything else to recoverable errors.
