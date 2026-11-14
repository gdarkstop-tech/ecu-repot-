"""Flash, test and report on the physical robot — run this on the laptop the ESP32-S3 is plugged into.

    python tools/verify_hardware.py                 # detect port, flash diag, flash robot, test, report
    python tools/verify_hardware.py --audio         # also test INMP441 mic + MAX98357A speaker (if wired)
    python tools/verify_hardware.py --port COM5 --skip-flash
    python tools/verify_hardware.py --sim firmware/sim/build/robot_sim   # protocol checks vs. simulator

Every result in the report is something this script observed, or something YOU confirmed when
asked (marked "confirmed by operator"). Checks that could not run are marked NOT TESTED with
the reason. Nothing is assumed to have passed.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import platform
import re
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from robotlink import RobotLink, likely_esp32_ports, list_ports  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware"

PASS, FAIL, SKIP, NOT_TESTED, INFO = "PASS", "FAIL", "SKIPPED", "NOT TESTED", "INFO"


# ====================================================================== report
@dataclass
class Check:
    group: str
    name: str
    status: str
    evidence: str = ""
    method: str = "automatic"  # automatic | confirmed by operator | operator declined
    hint: str = ""


@dataclass
class Report:
    started: str = field(default_factory=lambda: dt.datetime.now().isoformat(timespec="seconds"))
    target: str = "hardware"
    meta: dict = field(default_factory=dict)
    checks: list[Check] = field(default_factory=list)

    def add(self, group: str, name: str, status: str, evidence: str = "", method: str = "automatic",
            hint: str = "") -> Check:
        c = Check(group, name, status, evidence, method, hint)
        self.checks.append(c)
        icon = {PASS: "✔", FAIL: "✘", SKIP: "-", NOT_TESTED: "?", INFO: "i"}.get(status, " ")
        print(f"  [{icon}] {group} / {name}: {status}" + (f" — {evidence}" if evidence else ""))
        if hint and status == FAIL:
            print(f"      hint: {hint}")
        return c

    def write(self, directory: Path) -> Path:
        directory.mkdir(parents=True, exist_ok=True)
        stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        base = directory / f"verification-{self.target}-{stamp}"
        base.with_suffix(".json").write_text(json.dumps(asdict(self), indent=2, ensure_ascii=False), encoding="utf-8")
        lines = [
            f"# Robot verification report ({self.target})",
            "",
            f"- Started: {self.started}",
            f"- Host: {platform.system()} {platform.release()}, Python {platform.python_version()}",
        ]
        lines += [f"- {k}: {v}" for k, v in self.meta.items()]
        counts = {s: sum(1 for c in self.checks if c.status == s) for s in (PASS, FAIL, NOT_TESTED, SKIP)}
        lines += ["", "**Summary:** " + ", ".join(f"{k}: {v}" for k, v in counts.items()), ""]
        if self.target != "hardware":
            lines += ["> These results come from the HOST SIMULATOR, not the physical robot.", ""]
        group = None
        for c in self.checks:
            if c.group != group:
                group = c.group
                lines += ["", f"## {group}", "", "| Check | Result | Evidence | How |", "|---|---|---|---|"]
            ev = c.evidence.replace("|", "\\|") + (f" — hint: {c.hint}" if c.hint and c.status == FAIL else "")
            lines.append(f"| {c.name} | **{c.status}** | {ev} | {c.method} |")
        lines += [
            "", "## Still to verify by hand (cannot be automated over USB)", "",
            "- [ ] Standalone conversation on battery: unplug USB, power from the power bank, unmute with the",
            "      button, ask a question, hear the spoken answer, robot returns to listening (README section 7).",
            "- [ ] Arabic voice conversation end to end.",
            "- [ ] Robot does not answer its own voice (talk-over / echo test, README section 7).",
            "- [ ] Wi-Fi drop and recovery while running on battery.",
            "",
        ]
        base.with_suffix(".md").write_text("\n".join(lines), encoding="utf-8")
        return base.with_suffix(".md")


def ask(question: str, args) -> bool | None:
    """Ask the operator. Returns True/False, or None if skipped / non-interactive."""
    if args.no_confirm:
        return None
    while True:
        a = input(f"\n  >>> {question} [y]es / [n]o / [s]kip: ").strip().lower()
        if a in ("y", "yes"):
            return True
        if a in ("n", "no"):
            return False
        if a in ("s", "skip", ""):
            return None


def human(report: Report, group: str, name: str, question: str, args, hint: str = "") -> None:
    ans = ask(question, args)
    if ans is None:
        report.add(group, name, NOT_TESTED, "visual/audible check not confirmed", "operator declined")
    else:
        report.add(group, name, PASS if ans else FAIL, question, "confirmed by operator", hint)


# ====================================================================== tooling
def run(cmd: list[str], cwd: Path | None = None, timeout: int = 900) -> tuple[int, str]:
    print(f"    $ {' '.join(cmd)}")
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except FileNotFoundError as exc:
        return 127, str(exc)
    except subprocess.TimeoutExpired:
        return 124, "timeout"


def probe_chip(port: str) -> str | None:
    """Ask the chip on `port` what it is (esptool briefly resets that board; harmless)."""
    code, out = run([sys.executable, "-m", "esptool", "--port", port, "chip_id"], timeout=60)
    m = re.search(r"Chip (?:is|type:)\s*(.+)", out)
    return m.group(1).strip() if (code == 0 and m) else None


def choose_port(ports: list, probe) -> tuple[str | None, list[str]]:
    """Pick the ESP32-S3 among USB serial ports. Returns (port or None, notes).

    USB ids cannot tell an ESP32-S3 board on a CH340/CH343 bridge from an ESP32-CAM-MB
    (also CH340), so when more than one candidate exists every one is asked for its chip."""
    notes: list[str] = []
    if len(ports) == 1:
        return ports[0].device, notes
    s3 = []
    for p in ports:
        chip = probe(p.device)
        notes.append(f"{p.device}: {chip or 'no answer'}")
        if chip and "ESP32-S3" in chip:
            s3.append(p.device)
    if len(s3) == 1:
        return s3[0], notes
    return None, notes


def list_ports_and_exit() -> int:
    ports = list_ports()
    if not ports:
        print("No serial ports found.")
    for p in ports:
        vid = f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None else "----:----"
        chip = probe_chip(p.device) if p.kind not in ("unknown", "no USB id") else None
        print(f"{p.device:<14} {vid}  {p.kind:<40} chip: {chip or '-'}")
    return 0


def step_ports(report: Report, args) -> str | None:
    print("\n== 1. USB serial ports")
    ports = list_ports()
    for p in ports:
        vid = f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None else "----:----"
        print(f"    {p.device:<14} {vid}  {p.kind}  ({p.description})")
    if args.port:
        report.add("Connection", "Upload port", INFO, f"{args.port} (given with --port)")
        report.meta["COM port"] = args.port
        return args.port
    cands = likely_esp32_ports()
    if not cands:
        report.add("Connection", "ESP32-S3 USB port detected", FAIL, f"{len(ports)} serial ports, none recognised",
                   hint="Use a DATA USB-C cable (many are charge-only). Try the other USB-C connector on the board. "
                        "Windows: install the CP210x or CH343 driver if Device Manager shows an unknown device. "
                        "If no port appears at all, hold BOOT, tap RESET, release BOOT, then re-run.")
        return None
    if len(cands) > 1:
        print("    More than one ESP-style port: asking each board which chip it is...")
    port, notes = choose_port(cands, probe_chip)
    for n in notes:
        print(f"      {n}")
    if port is None:
        report.add("Connection", "ESP32-S3 USB port detected", FAIL, "; ".join(notes),
                   hint="Unplug the ESP32-CAM-MB (or other boards) and re-run, or pass --port COMx. "
                        "Run with --list-ports to see every port and its chip.")
        return None
    kind = next(p.kind for p in cands if p.device == port)
    report.add("Connection", "ESP32-S3 USB port detected", PASS, f"{port} — {kind}")
    report.meta["COM port"] = f"{port} ({kind})"
    return port


def step_esptool(report: Report, port: str) -> bool:
    """Returns True only if the chip on `port` is an ESP32-S3 (nothing is flashed otherwise)."""
    print("\n== 2. Chip, flash and PSRAM via esptool")
    code, out = run([sys.executable, "-m", "esptool", "--port", port, "flash_id"], timeout=120)
    if code != 0:
        report.add("Board", "esptool can talk to the chip", FAIL, out.strip().splitlines()[-1] if out else "no output",
                   hint="Close any serial monitor using the port (VS Code PlatformIO monitor too). "
                        "If it still fails, hold BOOT, tap RESET, release BOOT, and re-run.")
        return False
    report.add("Board", "esptool can talk to the chip", PASS)
    chip = re.search(r"Chip (?:is|type:)\s*(.+)", out)
    feats = re.search(r"Features:\s*(.+)", out)
    size = re.search(r"Detected flash size:\s*(\S+)", out)
    mac = re.search(r"MAC:\s*([0-9a-f:]{17})", out, re.I)
    report.meta["Chip (esptool)"] = chip.group(1).strip() if chip else "?"
    report.meta["Features (esptool)"] = feats.group(1).strip() if feats else "?"
    if mac:
        report.meta["MAC"] = mac.group(1)
    is_s3 = bool(chip and "ESP32-S3" in chip.group(1))
    report.add("Board", "Chip is ESP32-S3", PASS if is_s3 else FAIL, chip.group(1).strip() if chip else "not reported",
               hint="This port is a different board (an ESP32-CAM reports plain 'ESP32'). Nothing was flashed. "
                    "Use --port with the ESP32-S3's COM port.")
    report.add("Board", "Flash size 16 MB (esptool)", PASS if size and size.group(1) == "16MB" else FAIL,
               size.group(1) if size else "not reported")
    if feats:
        psram = re.search(r"PSRAM\s*(\d+)MB", feats.group(1))
        report.add("Board", "8 MB PSRAM reported by chip", PASS if psram and psram.group(1) == "8" else FAIL,
                   feats.group(1).strip())
    return is_s3


def flash(report: Report, env: str, port: str) -> bool:
    t0 = time.time()
    code, out = run([sys.executable, "-m", "platformio", "run", "-e", env, "-t", "upload", "--upload-port", port],
                    cwd=FIRMWARE, timeout=1200)
    ok = code == 0 and "SUCCESS" in out
    size = re.search(r"Flash: \[.*?\]\s+(\S+%) \(used (\d+) bytes", out)
    ev = f"{time.time() - t0:.0f} s" + (f", image {size.group(2)} bytes ({size.group(1)})" if size else "")
    report.add("Build & flash", f"Build + upload '{env}'", PASS if ok else FAIL, ev if ok else
               (out.strip().splitlines()[-1] if out else "no output"),
               hint="Run the same command by hand to see the full log: "
                    f"cd firmware && pio run -e {env} -t upload --upload-port {port}")
    return ok


# ====================================================================== diag firmware
def step_diag(report: Report, port: str, args) -> None:
    print("\n== 4. Diagnostic firmware output")
    link = RobotLink()
    link.open_serial(port)
    stages: dict[str, dict] = {}
    deadline = time.monotonic() + 15
    asked = False
    while time.monotonic() < deadline and "summary" not in stages:
        d = link.wait_for(lambda d: d.get("evt") == "diag", 1.0)
        if d:
            stages[d.get("stage", "?")] = d
        elif not asked and time.monotonic() > deadline - 8:
            link.send_raw("r")  # ask for a fresh report if the boot output was missed
            asked = True
    link.close()
    if not stages:
        report.add("Serial", "Diagnostic firmware talks over USB", FAIL, "no JSON received in 15 s",
                   hint="Press RESET on the board and re-run with --skip-flash.")
        return
    report.add("Serial", "Diagnostic firmware talks over USB", PASS, f"stages: {', '.join(stages)}")
    chip = stages.get("chip", {})
    if chip:
        report.meta["Chip (firmware)"] = f"{chip.get('model')} rev {chip.get('revision')}, {chip.get('cores')} cores @ {chip.get('cpu_mhz')} MHz"
    fl = stages.get("flash", {})
    if fl:
        ok = fl.get("physical_bytes") == 16777216 and fl.get("configured_bytes") == 16777216
        report.add("Board", "Flash 16 MB (JEDEC id, firmware config)", PASS if ok else FAIL,
                   f"physical {fl.get('physical_bytes')} B, configured {fl.get('configured_bytes')} B, "
                   f"mode {fl.get('mode')}, JEDEC {fl.get('jedec_id')}")
        report.meta["Flash"] = f"{fl.get('physical_bytes', 0) // 1048576} MB, {fl.get('mode')} @ {fl.get('speed_hz', 0) // 1000000} MHz"
    ps = stages.get("psram", {})
    if ps:
        ok = ps.get("found") and ps.get("size_bytes", 0) >= 8 * 1048576 - 65536 and ps.get("test") == "pass"
        report.add("Board", "PSRAM 8 MB found and read/write test", PASS if ok else FAIL,
                   f"size {ps.get('size_bytes')} B, test {ps.get('test')}, "
                   f"write {ps.get('write_mb_s')} MB/s, read {ps.get('read_mb_s')} MB/s",
                   hint="Board may not be N16R8, or PSRAM mode mismatch (platformio.ini memory_type).")
        report.meta["PSRAM"] = f"{ps.get('size_bytes', 0) // 1048576} MB, test {ps.get('test')}"
    lines = stages.get("i2c_lines", {})
    scan = stages.get("i2c_scan", {})
    if scan:
        found = scan.get("found", [])
        has = scan.get("oled_candidate")
        hint = ("No device ACKed. Check: OLED VCC->3V3, GND->GND, SDA->GPIO8, SCL->GPIO9 (not swapped), "
                f"module is I2C (4 pins). Pull-ups seen: SDA={lines.get('sda_external_pullup')}, "
                f"SCL={lines.get('scl_external_pullup')} (False = module unpowered or not connected).")
        report.add("OLED", "I2C scan finds OLED at 0x3C/0x3D", PASS if has else FAIL,
                   f"found {found}", hint=hint)
    ol = stages.get("oled", {})
    if ol:
        report.add("OLED", "SH1106 init + frame sent (I2C ACK)", PASS if ol.get("ok") else FAIL,
                   f"addr {ol.get('addr')}, frame {ol.get('frame_us')} us" if ol.get("ok") else ol.get("error", ""),
                   hint=ol.get("hint", ""))
        if ol.get("ok"):
            human(report, "OLED", "Diag test pattern visible", "Does the OLED show a full border, two blinking "
                  "eyes and 'ROBOT DIAG' text, with nothing cut off at the left/right edges?", args,
                  hint="If the picture is shifted by 2 pixels or has a garbage column, the module is SSD1306: "
                       "send {\"cmd\":\"config\",\"key\":\"oled_driver\",\"value\":\"ssd1306\"} later.")


# ====================================================================== robot firmware
def wait_hello(link: RobotLink, timeout: float) -> dict | None:
    d = link.wait_for(lambda d: d.get("evt") == "hello", timeout)
    if d is None:
        link.send("hello")
        d = link.wait_for(lambda d: d.get("evt") == "hello", 3)
    return d


def step_robot(report: Report, link: RobotLink, args) -> None:
    print("\n== 6. Robot firmware: protocol, state machine, face")
    G = "Robot firmware"
    hello = wait_hello(link, 6)
    if not hello:
        report.add(G, "hello received", FAIL, "no hello/ack within 9 s", hint="Press RESET; check the port.")
        return
    report.add(G, "hello received", PASS, f"fw {hello.get('fw')}, oled {hello.get('oled')} {hello.get('oled_addr')}")
    report.meta["Robot firmware"] = hello.get("fw")
    # Let STARTUP finish.
    link.wait_for(lambda d: d.get("evt") == "state" and d.get("from") == "STARTUP", 4)

    rtts = []
    for _ in range(10):
        t0 = time.monotonic()
        a = link.request("ping", 2)
        if a and a.get("ok"):
            rtts.append((time.monotonic() - t0) * 1000)
    report.add(G, "Ping round-trip (10x)", PASS if len(rtts) == 10 else FAIL,
               f"{len(rtts)}/10 ok, avg {sum(rtts) / max(1, len(rtts)):.1f} ms, max {max(rtts or [0]):.1f} ms")

    link.send("status")
    st = link.wait_for(lambda d: d.get("evt") == "status", 3) or {}
    report.add(G, "Status report", PASS if st else FAIL,
               f"state {st.get('state')}, muted {st.get('muted')}, heap {st.get('heap_free')}, psram {st.get('psram_free')}")
    report.add(G, "Microphone muted at startup", PASS if st.get("muted") and st.get("state") == "MUTED" else FAIL,
               f"state {st.get('state')}, muted {st.get('muted')}")

    # ---- protocol robustness
    link.send_raw("{this is not json")
    a = link.wait_for(lambda d: d.get("evt") == "ack" and d.get("ok") is False, 2)
    report.add(G, "Rejects malformed JSON", PASS if a and a.get("err") == "bad_json" else FAIL, str(a))
    a = link.request("launch_rockets", 2)
    report.add(G, "Rejects unknown command", PASS if a and a.get("err") == "unknown_cmd" else FAIL, str(a))
    link.send_raw("{" + '"pad":"' + "x" * 700 + '"}')
    a = link.wait_for(lambda d: d.get("evt") == "ack" and d.get("err") == "line_too_long", 2)
    report.add(G, "Bounded input buffer (700-byte line)", PASS if a else FAIL, "line_too_long reported" if a else "no error")
    a = link.request("ping", 2)
    report.add(G, "Recovers after bad input", PASS if a and a.get("ok") else FAIL)
    a = link.request("state", 2, state="LISTENING")
    report.add(G, "LISTENING refused while muted", PASS if a and a.get("err") == "mic_muted" else FAIL, str(a))

    # ---- state machine
    def to_state(name: str, **kw) -> bool:
        a = link.request("state", 3, state=name, **kw)
        return bool(a and a.get("ok"))

    a = link.request("mute", 2, on=False)
    report.add(G, "Unmute -> IDLE", PASS if a and a.get("state") == "IDLE" else FAIL, str(a))
    seq = ["LISTENING", "PROCESSING", "SPEAKING", "IDLE"]
    ok = all(to_state(s) for s in seq)
    report.add(G, "Transitions IDLE->LISTENING->PROCESSING->SPEAKING->IDLE", PASS if ok else FAIL)
    to_state("PROCESSING", ms=1500)
    d = link.wait_for(lambda d: d.get("evt") == "state" and d.get("from") == "PROCESSING" and d.get("reason") == "timeout", 4)
    report.add(G, "PROCESSING times out back to rest (recovery)", PASS if d else FAIL, str(d))
    a = link.request("show", 2, kind="letter", value="A", caption="A is for Apple", ms=1500)
    report.add(G, "LEARNING content (letter card)", PASS if a and a.get("state") == "LEARNING" else FAIL, str(a))
    time.sleep(1.8)
    a = link.request("mute", 2, on=True)
    report.add(G, "Mute -> MUTED", PASS if a and a.get("state") == "MUTED" else FAIL, str(a))

    # ---- selftest = every expression and screen, observed by the operator
    link.drain()
    link.request("selftest", 2)
    names = []
    t_end = time.monotonic() + 60
    while time.monotonic() < t_end:
        d = link.wait_for(lambda d: d.get("evt") == "selftest", 5)
        if not d:
            break
        if d.get("done"):
            break
        names.append(d.get("name"))
        print(f"      showing: {d.get('name')}")
    report.add(G, "Selftest ran all expressions", PASS if len(names) >= 15 else FAIL, ", ".join(names))
    if not args.target_sim:
        human(report, "OLED", "Expressions animate smoothly", "Did you see idle, blink, happy, listening, thinking, "
              "speaking, surprised, sad, sleeping, muted, error, then letter/number/sum/word cards — smooth, no "
              "flicker?", args)
        link.request("show", 2, kind="letter", value="ب", caption="ﺔﻄﺒ", rtl=True, ms=6000)
        human(report, "OLED", "Arabic renders", "Is a large Arabic letter (ب) shown with an Arabic word below it?", args)
        link.request("clear", 2)


# ====================================================================== audio
def step_audio(report: Report, link: RobotLink, args) -> None:
    print("\n== 7. Audio hardware")
    G = "Audio"
    if not args.audio:
        report.add(G, "INMP441 microphone", NOT_TESTED, "not requested (run with --audio once wired)")
        report.add(G, "MAX98357A speaker", NOT_TESTED, "not requested (run with --audio once wired)")
        return
    results = {}
    for label, prompt in (("quiet", "Stay QUIET for 3 seconds"), ("speech", "Now TALK to the robot for 3 seconds")):
        input(f"\n  >>> {prompt}, press Enter to start...")
        link.request("mic_test", 2, ms=3000)
        d = link.wait_for(lambda d: d.get("evt") == "mic_test", 6)
        results[label] = d or {}
        print(f"      {label}: {d}")
    q, s = results.get("quiet", {}), results.get("speech", {})
    alive = s.get("verdict") == "signal"
    gain = (s.get("rms_dbfs", -120) - q.get("rms_dbfs", -120)) if alive else 0
    report.add(G, "INMP441 delivers real samples", PASS if alive else FAIL,
               f"verdict {s.get('verdict')}, frames {s.get('frames')}, zero frames {s.get('zero_frames')}",
               hint="all_zero/stuck: check SCK=GPIO5, WS=GPIO6, SD=GPIO7, VDD=3V3, GND, L/R->GND; or try "
                    "config mic_channel=right then reboot.")
    report.add(G, "Microphone hears speech (speech vs quiet)", PASS if alive and gain >= 6 else FAIL,
               f"quiet {q.get('rms_dbfs')} dBFS, speech {s.get('rms_dbfs')} dBFS, difference {gain:.1f} dB",
               hint="Speak closer, or raise mic_gain (config).")
    link.request("tone", 2, hz=440, ms=1200)
    time.sleep(1.5)
    human(report, G, "Speaker plays test tone", "Did you hear a clear 1-second beep from the robot's speaker?", args,
          hint="Check BCLK=GPIO15, LRC=GPIO16, DIN=GPIO17, SD=GPIO18, VIN=5V, GND, speaker on the amp terminals.")
    input("\n  >>> Loopback: after Enter, say a short sentence; the robot plays it back. Press Enter...")
    link.request("loopback", 2, ms=3000)
    link.wait_for(lambda d: d.get("evt") == "loopback", 10)
    human(report, G, "Mic -> speaker loopback", "Did you hear your own voice played back?", args)


# ====================================================================== network
def step_network(report: Report, link: RobotLink, args) -> None:
    print("\n== 8. Wi-Fi and AI gateway")
    G = "Network"
    link.send("config_get")
    cfg = link.wait_for(lambda d: d.get("evt") == "config", 3) or {}
    if not cfg.get("wifi_ssid"):
        report.add(G, "Wi-Fi", NOT_TESTED, "no Wi-Fi configured (tools/robot_console.py provision)")
        report.add(G, "Gateway turn", NOT_TESTED, "no Wi-Fi configured")
        return
    connected = None
    for _ in range(10):
        link.send("status")
        st = link.wait_for(lambda d: d.get("evt") == "status", 3) or {}
        if st.get("wifi", {}).get("connected"):
            connected = st
            break
        time.sleep(2)
    report.add(G, "Wi-Fi connects", PASS if connected else FAIL,
               f"ssid {cfg.get('wifi_ssid')}, rssi {connected['wifi']['rssi'] if connected else '?'} dBm",
               hint="Check SSID/password (2.4 GHz only — the ESP32-S3 has no 5 GHz radio).")
    if not connected or not cfg.get("device_token_set") or not cfg.get("gateway_url"):
        report.add(G, "Gateway turn", NOT_TESTED, "gateway URL / device token not provisioned")
        return
    link.request("mute", 2, on=False)
    link.drain()
    t0 = time.monotonic()
    a = link.request("turn_text", 3, text="What is seven plus five?", lang="en")
    if not a or not a.get("ok"):
        report.add(G, "Gateway turn (text -> AI -> speech)", FAIL, str(a))
        return
    stages = []
    while time.monotonic() - t0 < 40:
        d = link.wait_for(lambda d: d.get("evt") in ("turn", "state"), 5)
        if not d:
            break
        if d.get("evt") == "turn":
            stages.append(d.get("stage"))
            if d.get("stage") in ("spoken", "failed"):
                break
    ok = "reply" in stages
    report.add(G, "Gateway turn (text -> AI -> speech)", PASS if ok else FAIL,
               f"stages {stages}, {time.monotonic() - t0:.1f} s")
    if ok and args.audio:
        human(report, G, "Spoken reply heard", "Did the robot SAY that 7 plus 5 is 12?", args)
    link.request("mute", 2, on=True)


# ====================================================================== main
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--sim", help="path to firmware/sim/build/robot_sim (protocol checks only)")
    ap.add_argument("--skip-flash", action="store_true")
    ap.add_argument("--skip-diag", action="store_true")
    ap.add_argument("--audio", action="store_true", help="the INMP441 and MAX98357A are wired")
    ap.add_argument("--backup-flash", action="store_true", help="save the board's current 16 MB flash first")
    ap.add_argument("--no-confirm", action="store_true", help="non-interactive (visual checks = NOT TESTED)")
    ap.add_argument("--report-dir", default=str(ROOT / "reports"))
    ap.add_argument("--list-ports", action="store_true", help="list serial ports and the chip on each, then exit")
    args = ap.parse_args()
    if args.list_ports:
        return list_ports_and_exit()
    args.target_sim = bool(args.sim)

    report = Report(target="simulator" if args.sim else "hardware")
    try:
        if args.sim:
            link = RobotLink()
            link.open_sim(args.sim)
            step_robot(report, link, args)
            link.close()
        else:
            port = step_ports(report, args)
            if port and not step_esptool(report, port):
                print("\n  Stopping before flashing: the chip on this port is not a confirmed ESP32-S3.")
                port = None
            if port:
                if args.backup_flash:
                    out = Path(args.report_dir) / f"flash_backup_{int(time.time())}.bin"
                    out.parent.mkdir(parents=True, exist_ok=True)
                    code, _ = run([sys.executable, "-m", "esptool", "--port", port, "--baud", "921600",
                                   "read_flash", "0", "0x1000000", str(out)], timeout=900)
                    report.add("Build & flash", "Backup of existing flash", PASS if code == 0 else FAIL, str(out))
                if not args.skip_diag:
                    print("\n== 3. Flash diagnostic firmware")
                    if args.skip_flash or flash(report, "diag", port):
                        time.sleep(2.5)
                        step_diag(report, port, args)
                if not args.skip_flash:
                    print("\n== 5. Flash robot firmware")
                    if not flash(report, "robot", port):
                        raise SystemExit(report.write(Path(args.report_dir)))
                    time.sleep(2.5)
                link = RobotLink()
                link.open_serial(port)
                step_robot(report, link, args)
                step_audio(report, link, args)
                step_network(report, link, args)
                link.close()
    except KeyboardInterrupt:
        report.add("Run", "Interrupted", INFO, "stopped by operator")
    path = report.write(Path(args.report_dir))
    fails = sum(1 for c in report.checks if c.status == FAIL)
    print(f"\nReport written to {path}  ({fails} failed)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
