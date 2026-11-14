# Wiring — ESP32-S3-DevKitC-1 N16R8

Source of truth for pins: [`firmware/include/board_pins.h`](../firmware/include/board_pins.h).
**Only the OLED and the mute button are needed for phase 1.** Everything else is optional until
its phase.

> Power off (unplug USB) before changing wiring. All signal lines are **3.3 V**. Never connect a
> 5 V signal to a GPIO.

## Phase 1 — OLED (required)

| SH1106 1.3" OLED pin | ESP32-S3 pin | Notes |
|---|---|---|
| VCC | **3V3** | Not 5 V |
| GND | GND | |
| SDA | **GPIO8** | I2C data |
| SCL | **GPIO9** | I2C clock |

The module's own pull-up resistors are used (most modules have 4.7–10 kΩ). Address is usually
`0x3C` (sometimes `0x3D`, selected by a jumper/resistor on the back). The firmware tries both.

## Mute button (PBS-33B)

| Button pin | ESP32-S3 pin |
|---|---|
| one leg | **GPIO4** |
| other leg | GND |

Internal pull-up is enabled, so no resistor is needed. **Short press** = mute/unmute the
microphone. **Long press (1.2 s)** = next activity in offline mode. If the button is not
connected the pin reads "not pressed", which is safe.

## Phase 2 — Microphone: INMP441 (I2S0, input)

| INMP441 pin | ESP32-S3 pin | Notes |
|---|---|---|
| VDD | 3V3 | |
| GND | GND | |
| SCK | **GPIO5** | bit clock |
| WS | **GPIO6** | word select |
| SD | **GPIO7** | data from the mic |
| L/R | **GND** | selects the LEFT channel |

If `mic_test` reports `all_zero`, re-check the wiring; if the wiring is right, send
`set mic_channel right` in the console and reboot (some modules/boards swap channels).

## Phase 2 — Speaker: MAX98357A (I2S1, output) + 4 Ω 3 W speaker

| MAX98357A pin | ESP32-S3 pin | Notes |
|---|---|---|
| VIN | **5V** (VBUS) | The amp needs 5 V for full volume; logic stays 3.3 V-compatible |
| GND | GND | |
| BCLK | **GPIO15** | |
| LRC | **GPIO16** | |
| DIN | **GPIO17** | |
| SD | **GPIO18** | Firmware drives HIGH while playing, LOW otherwise (silences the amp, avoids hiss) |
| GAIN | leave unconnected | 9 dB default; tie to GND for 12 dB if too quiet |
| Speaker + / − | speaker terminals | 4 Ω, 3 W |

Keep the speaker facing away from the microphone and at least ~10 cm from it. This works
together with the firmware's echo guard (the mic is switched off while the robot speaks).

## Power (standalone operation)

- Power the DevKitC from the JOYROOM JR-PBF12 power bank through the board's USB-C port.
- A speaker at loud volume draws peaks around 0.6–0.8 A at 5 V; Wi-Fi adds ~0.3 A peaks. Use a
  good USB-C cable. If the robot resets when it starts talking (brown-out), lower `volume` or
  add a 470–1000 µF capacitor across the MAX98357A VIN/GND.
- Some power banks switch off when the current draw is very low. If the robot powers down while
  idle, check the power bank's "low-current mode" (often a double-press of its button).

## Planned (not used by the current firmware)

| Module | Pins | Notes |
|---|---|---|
| MicroSD (SPI) | CS 10, MOSI 11, SCK 12, MISO 13 | ESP32-S3 FSPI default pins |
| VL53L0X distance | I2C on GPIO8/9 (addr 0x29), XSHUT 14 | Shares the OLED bus |
| MG90S servo | PWM GPIO21 | Power the servo from 5 V, **not** 3V3; common GND |
| DRV8833 motors | AIN1 39, AIN2 40, BIN1 41, BIN2 42 | Separate motor supply recommended |
| AI-Thinker ESP32-CAM | Wi-Fi to the gateway; optional UART TX 1 / RX 2 | It is a separate computer with its own firmware |

## Pins you must not use on an N16R8

GPIO26–32 (flash), GPIO33–37 (octal PSRAM), GPIO19/20 (native USB), GPIO43/44 (UART0),
GPIO0/3/45/46 (strapping), GPIO38/48 (on-board RGB LED).
