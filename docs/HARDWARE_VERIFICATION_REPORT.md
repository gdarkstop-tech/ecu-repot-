# Hardware verification report — current status

**Date:** 2026-11-14 · **Firmware:** 0.2.0 · **Gateway:** 0.2.0

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
| Firmware unit tests (host) | **73/73 pass**: state machine 14, protocol/framing/UTF-8 18, robot coordinator 12, VAD + capture 7, conversation loop incl. echo guard/timeouts/mute 10, offline tutor 4, firmware↔gateway contract 8 |
| Gateway tests | **55/55 pass**: auth, size/format limits, rate limit, one-time device-bound audio, math engine (EN/AR words, Arabic-Indic digits), tutor modes/difficulty/quiz, safety filters, LLM output cleaning + arithmetic correction, Claude request shape, eSpeak EN/AR synthesis |
| Contract | The firmware's own C++ parser reads 8 real gateway replies (EN/AR math, alphabet, story, vocab, ignore, demo) and rejects hostile ones (foreign audio URL, path traversal, bad format/rate, unknown emotion, invalid UTF-8) |
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
- [ ] Wi-Fi connects (2.4 GHz); gateway text turn produces reply → spoken

**Phase 4 — standalone (manual, laptop disconnected)** — see README section 7
- [ ] Power bank only: boot, unmute, ask a question by voice, hear the answer, robot listens again
- [ ] Arabic voice conversation
- [ ] Robot does not answer its own voice
- [ ] Wi-Fi off → offline cards; Wi-Fi back → conversations resume

**The voice AI requirement is not complete until the Phase 4 items pass on the physical robot.**
