// ALIA Robot — GPIO assignments for ESP32-S3-DevKitC-1 N16R8.
//
// Single source of truth for pin usage. docs/WIRING.md mirrors this file.
//
// Pins that MUST NOT be used on an N16R8 board:
//   GPIO26-32  internal SPI flash
//   GPIO33-37  internal octal PSRAM (the "R8" in N16R8)
//   GPIO19/20  native USB D-/D+ (USB-Serial/JTAG)
//   GPIO43/44  UART0 TX/RX (USB-UART bridge port, boot log)
//   GPIO0/3/45/46 strapping pins (boot mode / JTAG / flash voltage)
//   GPIO38/48  on-board RGB LED (v1.1 uses 38, v1.0 uses 48) — left free
#pragma once

// ---------------------------------------------------------------- display bus
// SH1106 1.3" OLED (I2C 0x3C, sometimes 0x3D). VL53L0X (0x29) will share this bus.
#define PIN_I2C_SDA 8
#define PIN_I2C_SCL 9
#define I2C_BUS_HZ 400000

// ---------------------------------------------------------------- user input
// PBS-33B momentary push button between GPIO4 and GND (internal pull-up).
// Short press toggles software microphone mute.
#define PIN_MUTE_BUTTON 4

// ---------------------------------------------------------------- audio in
// INMP441 I2S MEMS microphone on I2S0 (RX). L/R pin tied to GND = left channel.
#define PIN_MIC_SCK 5   // INMP441 SCK  (bit clock)
#define PIN_MIC_WS 6    // INMP441 WS   (word select / LRCLK)
#define PIN_MIC_SD 7    // INMP441 SD   (data out of the mic)

// ---------------------------------------------------------------- audio out
// MAX98357A I2S class-D amplifier on I2S1 (TX), 4 ohm 3 W speaker.
#define PIN_AMP_BCLK 15
#define PIN_AMP_LRC 16
#define PIN_AMP_DIN 17
#define PIN_AMP_SD 18   // amplifier enable: HIGH = on (left channel), LOW = shutdown

// ---------------------------------------------------------------- planned, not used yet
// MicroSD (SPI, ESP32-S3 FSPI default pins)
#define PIN_SD_CS 10
#define PIN_SD_MOSI 11
#define PIN_SD_SCK 12
#define PIN_SD_MISO 13
// VL53L0X distance sensor (I2C shared with OLED) — shutdown line
#define PIN_TOF_XSHUT 14
// MG90S servo (LEDC PWM 50 Hz). Power the servo from 5 V, NOT the 3.3 V rail.
#define PIN_SERVO 21
// DRV8833 dual H-bridge for two TT gear motors
#define PIN_MOTOR_AIN1 39
#define PIN_MOTOR_AIN2 40
#define PIN_MOTOR_BIN1 41
#define PIN_MOTOR_BIN2 42
// AI-Thinker ESP32-CAM: separate MCU. Planned link is Wi-Fi (camera -> gateway),
// optional UART1 control link on GPIO1 (TX) / GPIO2 (RX).
#define PIN_CAM_UART_TX 1
#define PIN_CAM_UART_RX 2
