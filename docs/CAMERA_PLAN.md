# ESP32-CAM integration — status and plan

## Status today

**Not implemented.** There is no ESP32-CAM firmware and no camera code on the ESP32-S3 or in the
gateway. What exists: GPIO1/GPIO2 on the ESP32-S3 are reserved for a camera link
(`firmware/include/board_pins.h`), and the tutor honestly answers "my camera is not connected yet"
when a child asks "what is this?". None of this affects the OLED / audio / AI work.

Toolchain support is confirmed: PlatformIO's `esp32cam` board (AI Thinker ESP32-CAM, 4 MB flash,
PSRAM) and the `esp32-camera` driver are part of the pinned `espressif32@6.13.0` platform.

## Recommended design: the camera is a UART peripheral of the ESP32-S3

```
child: "What is this?"  ─▶ ESP32-S3 ─▶ gateway (STT + tutor: vision intent) ─▶ reply {"action":"capture"}
ESP32-S3 ── UART 921600 ──▶ ESP32-CAM: capture VGA JPEG ──▶ bytes back to the S3 (PSRAM)
ESP32-S3 ── HTTPS ──▶ gateway /v1/turns/{id}/vision (JPEG) ─▶ vision model ─▶ spoken answer
```

Why not put the camera on Wi-Fi by itself: it would need its own credentials and Wi-Fi setup, it
could stream images somewhere, and it uses more power. Over UART it has **no network access at
all**, the robot keeps a single revocable credential, and images are taken only when the child
asks. A VGA JPEG (≈15–40 KB) crosses a 921600-baud link in about 0.2–0.5 s.

## Wiring (planned)

| ESP32-S3 | ESP32-CAM (AI-Thinker) | Notes |
|---|---|---|
| GPIO1 (UART1 TX) | GPIO13 (UART2 RX) | |
| GPIO2 (UART1 RX) | GPIO14 (UART2 TX) | |
| GND | GND | common ground is required |
| — | 5V | from the power bank (not the S3's 3V3); the CAM draws 200–300 mA peaks |

GPIO13/14 are the camera board's microSD pins, so its SD slot stays unused. The CAM's own
UART0 (GPIO1/GPIO3) stays free for the ESP32-CAM-MB programmer.

## Exactly what needs to be added

1. **Camera firmware** — new PlatformIO project `camera/` (env `esp32cam`):
   - OV2640 init with the AI-Thinker pin map (PWDN 32, XCLK 0, SIOD 26, SIOC 27, Y2–Y9 5/18/19/21/36/39/34/35,
     VSYNC 25, HREF 23, PCLK 22), VGA JPEG, frame buffer in PSRAM.
   - UART2 protocol (newline JSON + binary payload): `ping`, `status`, `capture{res,quality}` →
     `{"evt":"frame","len":N,"crc32":X}` followed by N JPEG bytes; error events.
   - Sensor powered down (PWDN) between captures; flash LED (GPIO4) off unless explicitly requested.
   - A diag mode that prints chip/PSRAM/sensor id over USB for first bring-up.
2. **ESP32-S3 firmware** — `robot_hw/hw_camera` (UART1 driver, 64 KB PSRAM frame buffer, CRC
   check, timeouts, reconnect) and a portable, unit-tested frame parser in `robot_logic`;
   debug commands `cam_ping` / `cam_capture`; a "looking" expression and an on-screen camera
   indicator while a picture is taken; conversation support for a reply carrying
   `"action":"capture"`. Muting the robot also disables the camera.
3. **Gateway** — `POST /v1/turns/{id}/vision` (JPEG ≤ 200 KB, device-bound to the turn that asked),
   a vision provider (Claude accepts images; the provider interface stays swappable), child-safety
   checks on the answer, the tutor's vision intent returning `capture` instead of "not connected",
   images never stored, separate rate limit.
4. **Tests** — frame parser unit tests, gateway vision tests with a fixture JPEG, a contract
   fixture, and `verify_hardware.py --camera` (ping, capture, JPEG header check, operator confirms
   the subject).
5. **Docs** — wiring, flashing with the ESP32-CAM-MB, privacy notes for parents.

## Flashing the ESP32-CAM (when the firmware exists)

Plug the CAM onto the ESP32-CAM-MB and connect the MB by USB (it appears as a CH340 COM port).
If upload does not start automatically: hold **IO0** on the MB, press **RST** on the CAM, release
IO0. **Unplug the ESP32-CAM-MB while flashing the ESP32-S3**, or always pass `--port` — the
verification script refuses to flash a board that is not an ESP32-S3.

## Order of work

OLED + board (phase 1) → microphone/speaker (phase 2) → Wi-Fi + gateway conversation (phase 3)
→ standalone power-bank test (phase 4) → **camera** (phase 5).
