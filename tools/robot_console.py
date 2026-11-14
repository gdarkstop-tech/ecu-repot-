"""Interactive USB console for the robot (development, testing and first-time setup).

    python tools/robot_console.py                  # auto-detect the robot's port
    python tools/robot_console.py --port COM5
    python tools/robot_console.py provision        # guided Wi-Fi + gateway + token setup
    python tools/robot_console.py --sim firmware/sim/build/robot_sim

Console commands (anything starting with '{' is sent as raw JSON):
    face happy | state LISTENING | text Hello | show letter A | say Hi there | mute on|off
    status | ping | selftest | mic 3000 | tone 440 | loopback | wifi | ask <question> | config_get
    set <key> <value>   (wifi_ssid, gateway_url, lang, child_age, volume, mic_gain, oled_driver, mic_channel)
    secret <key>        (wifi_pass / device_token, typed hidden)
    reboot | quit
"""
from __future__ import annotations

import argparse
import getpass
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from robotlink import Event, RobotLink, likely_esp32_ports  # noqa: E402

QUIET = {"pong"}


def show(ev: Event) -> None:
    d = ev.data
    if d is None:
        print(f"  ~ {ev.raw}")
        return
    if d.get("evt") == "ack" and d.get("ok"):
        return  # successful acks are noise in the console
    print(f"  < {json.dumps(d, ensure_ascii=False)}")


def connect(args) -> RobotLink:
    link = RobotLink(on_event=show)
    if args.sim:
        link.open_sim(args.sim)
        print(f"Connected to simulator {args.sim}")
        return link
    port = args.port
    if not port:
        cands = likely_esp32_ports()
        if not cands:
            sys.exit("No ESP32-S3 port found. Check the USB cable/driver, or pass --port.")
        port = cands[0].device
    link.open_serial(port)
    print(f"Connected to {port}. Type 'help' for commands.")
    return link


def provision(link: RobotLink) -> None:
    print("Robot provisioning. Values are stored in the robot's flash (NVS).")
    print("Secrets are typed hidden and are never printed by the robot.\n")
    ssid = input("Wi-Fi network name (2.4 GHz): ").strip()
    pw = getpass.getpass("Wi-Fi password (hidden): ")
    url = input("AI gateway URL (https://...): ").strip()
    token = getpass.getpass("Device token from 'python -m robot_gateway.devices add' (hidden): ").strip()
    lang = input("Language [auto/en/ar] (auto): ").strip() or "auto"
    age = input("Child's age [3-12] (6): ").strip() or "6"
    for key, value in [("wifi_ssid", ssid), ("wifi_pass", pw), ("gateway_url", url), ("device_token", token),
                       ("lang", lang), ("child_age", age)]:
        a = link.request("config", 3, key=key, value=value)
        status = "ok" if a and a.get("ok") else f"REJECTED ({a.get('err') if a else 'no answer'})"
        print(f"  {key}: {status}")
    print("\nWaiting for Wi-Fi...")
    for _ in range(15):
        link.send("status")
        st = link.wait_for(lambda d: d.get("evt") == "status", 3) or {}
        if st.get("wifi", {}).get("connected"):
            print(f"  Wi-Fi connected, IP {st['wifi'].get('ip')}, RSSI {st['wifi'].get('rssi')} dBm")
            break
        time.sleep(2)
    else:
        print("  Wi-Fi not connected yet — check the name/password (2.4 GHz only).")


def repl(link: RobotLink) -> None:
    aliases = {"mic": ("mic_test", "ms"), "tone": ("tone", "hz"), "wifi": ("wifi_scan", None)}
    while True:
        try:
            line = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            break
        if not line:
            continue
        if line in ("quit", "exit"):
            break
        if line == "help":
            print(__doc__)
            continue
        if line.startswith("{"):
            link.send_raw(line)
            continue
        cmd, _, rest = line.partition(" ")
        rest = rest.strip()
        if cmd == "face":
            link.send("face", expr=rest or "happy")
        elif cmd == "state":
            link.send("state", state=rest.upper())
        elif cmd == "text":
            link.send("text", text=rest)
        elif cmd == "say":
            link.send("say", text=rest)
        elif cmd == "show":
            kind, _, value = rest.partition(" ")
            link.send("show", kind=kind, value=value)
        elif cmd == "mute":
            link.send("mute", on=rest != "off")
        elif cmd == "ask":
            link.send("turn_text", text=rest)
        elif cmd == "set":
            key, _, value = rest.partition(" ")
            link.send("config", key=key, value=value)
        elif cmd == "secret":
            link.send("config", key=rest, value=getpass.getpass(f"{rest} (hidden): "))
        elif cmd == "loopback":
            print("  speak after the beep...")
            link.send("loopback", ms=3000)
        elif cmd in aliases:
            name, param = aliases[cmd]
            if param and rest.isdigit():
                link.send(name, **{param: int(rest)})
            else:
                link.send(name)
        else:
            link.send(cmd)
        time.sleep(0.15)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", nargs="?", choices=["console", "provision"], default="console")
    ap.add_argument("--port")
    ap.add_argument("--sim")
    args = ap.parse_args()
    link = connect(args)
    try:
        if args.action == "provision":
            link.on_event = None
            provision(link)
        else:
            repl(link)
    finally:
        link.close()


if __name__ == "__main__":
    main()
