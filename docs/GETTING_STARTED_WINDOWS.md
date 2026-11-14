# Getting started on Windows (VS Code + PlatformIO)

The project lives on GitHub: **https://github.com/gdarkstop-tech/ecu-repot-** (public), branch
**`claude/vigilant-gauss-lvs43y`** (also the repository's default branch).

## 1. Get the code — choose A or B

**A. With Git** (recommended: easy to update later). Install Git for Windows from git-scm.com,
then in PowerShell:

```powershell
cd $HOME\Documents
```

```powershell
git clone --branch claude/vigilant-gauss-lvs43y https://github.com/gdarkstop-tech/ecu-repot-.git robot
```

```powershell
cd robot
```

To get later updates: `git pull` inside the `robot` folder.

**B. Without Git:** download
https://github.com/gdarkstop-tech/ecu-repot-/archive/refs/heads/claude/vigilant-gauss-lvs43y.zip ,
right-click → **Extract All…** into `Documents`, and rename the extracted folder to `robot`.

Keep the path short and without non-English characters (e.g. `C:\Users\you\Documents\robot`);
very long paths can break PlatformIO builds on Windows.

## 2. Install the Python tools (once)

Install **Python 3.11+** from python.org (tick **Add python.exe to PATH**). Then in PowerShell, in
the `robot` folder:

```powershell
powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1
```

This creates `robot\.venv` with PlatformIO, esptool and pyserial. No activation is needed: the
commands below call `.venv\Scripts\python.exe` directly.

## 3. Open the firmware in VS Code

1. VS Code → **File → Open Folder…** → select **`robot\firmware`** (the folder that contains
   `platformio.ini` — not the top `robot` folder, otherwise PlatformIO does not see the project).
2. Wait for the PlatformIO icon (ant head) in the left bar. The **first build downloads the
   ESP32-S3 toolchain and libraries (several hundred MB, a few minutes)** — this is expected.
3. In the blue status bar at the bottom, click the environment switcher (shows `env:robot`) and
   pick **`env:diag`** for the first test. Then: ✓ = Build, → = Upload, plug icon = Serial Monitor.

The two environments: `diag` = small diagnostic firmware (do this first), `robot` = the robot.
Board settings are already in `platformio.ini`: ESP32-S3-DevKitC-1, 16 MB QIO flash, 8 MB OPI
PSRAM, `default_16MB.csv` partitions. OLED pins are SDA = GPIO8, SCL = GPIO9
(`include/board_pins.h`).

## 4. Find the COM port

- Plug the ESP32-S3 in with a **data** USB-C cable. The DevKitC has two USB-C connectors; either works.
  - "USB" connector → **USB Serial Device (COMx)** (native, no driver needed)
  - "UART"/"COM" connector → **Silicon Labs CP210x (COMx)** or **USB-Enhanced-SERIAL CH343 (COMx)**
    (install that driver if Device Manager shows an unknown device)
- Check in **Device Manager → Ports (COM & LPT)**, or let the tool tell you (it asks each board
  which chip it is):

```powershell
.venv\Scripts\python.exe tools\verify_hardware.py --list-ports
```

- **Unplug the ESP32-CAM-MB while working on the ESP32-S3** (it is also a CH340 COM port). The
  script refuses to flash a board that is not an ESP32-S3, but unplugging avoids confusion.

## 5. Flash and test (one command)

Close VS Code's Serial Monitor first (only one program can use the COM port), then from the
`robot` folder:

```powershell
.venv\Scripts\python.exe tools\verify_hardware.py
```

Or with an explicit port:

```powershell
.venv\Scripts\python.exe tools\verify_hardware.py --port COM5
```

It reads the chip/flash/PSRAM, flashes `diag`, reads the I2C scan and OLED result, flashes
`robot`, runs the protocol/state/face checks, asks you to confirm what you see on the screen, and
writes `reports\verification-hardware-<time>.md`. It only reports what it actually observed.

If anything fails, the report says what and why, with a hint (wiring, driver, port).

## 6. If you prefer clicking in VS Code

With `env:diag` selected: **Upload** (→), then **Serial Monitor**. You should see JSON lines
such as `{"evt":"diag","stage":"oled","ok":true,...}` and a border, two blinking eyes and
"ROBOT DIAG" on the OLED. Type `r` + Enter in the monitor for a fresh report. Then switch to
`env:robot` and Upload again.

## Next steps

Microphone + speaker: README section 3. AI gateway: section 4. Provisioning: section 5.
Standalone power-bank test: section 7. Camera: [CAMERA_PLAN.md](CAMERA_PLAN.md).
