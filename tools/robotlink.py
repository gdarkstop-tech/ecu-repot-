"""Connection to the robot's USB debug link (or to the host simulator).

Newline-delimited JSON both ways. Commands carry an id; the robot answers with
{"evt":"ack","id":...}. Everything else the robot prints is an event; non-JSON lines (boot ROM
messages, panics) are kept as raw text for diagnostics.
"""
from __future__ import annotations

import json
import queue
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Callable

# USB vendor:product IDs of the ESP32-S3 native port and common USB-UART bridges.
KNOWN_USB = {
    (0x303A, 0x1001): "ESP32-S3 native USB (USB-Serial/JTAG)",
    (0x303A, 0x0002): "ESP32-S3 native USB (TinyUSB CDC)",
    (0x10C4, 0xEA60): "Silicon Labs CP210x USB-UART bridge",
    (0x1A86, 0x55D3): "WCH CH343 USB-UART bridge",
    (0x1A86, 0x7523): "WCH CH340 USB-UART bridge",
    (0x0403, 0x6001): "FTDI FT232 USB-UART bridge",
}


@dataclass
class PortInfo:
    device: str
    description: str
    vid: int | None
    pid: int | None
    kind: str


def list_ports() -> list[PortInfo]:
    from serial.tools import list_ports as lp

    out = []
    for p in lp.comports():
        kind = KNOWN_USB.get((p.vid, p.pid), "unknown") if p.vid is not None else "no USB id"
        out.append(PortInfo(p.device, p.description or "", p.vid, p.pid, kind))
    return out


def likely_esp32_ports() -> list[PortInfo]:
    ports = [p for p in list_ports() if p.kind != "unknown" and p.kind != "no USB id"]
    # Prefer the native ESP32-S3 port, then bridges.
    return sorted(ports, key=lambda p: 0 if p.vid == 0x303A else 1)


@dataclass
class Event:
    t: float
    data: dict | None
    raw: str


@dataclass
class RobotLink:
    events: "queue.Queue[Event]" = field(default_factory=queue.Queue)
    history: list[Event] = field(default_factory=list)
    on_event: Callable[[Event], None] | None = None
    _next_id: int = 1
    _closed: bool = False

    # ---- transports
    def open_serial(self, port: str, baud: int = 115200, retries: int = 20) -> None:
        import serial

        last = None
        for _ in range(retries):  # the native USB port re-enumerates after a reset
            try:
                s = serial.Serial()
                s.port, s.baudrate, s.timeout = port, baud, 0.1
                s.dtr = False  # do not toggle EN/IO0 (would reset the board on many dev kits)
                s.rts = False
                s.open()
                self._ser = s
                self._write = lambda b: (s.write(b), s.flush())
                threading.Thread(target=self._read_serial, daemon=True).start()
                return
            except Exception as exc:  # noqa: BLE001 - report the last error
                last = exc
                time.sleep(0.5)
        raise RuntimeError(f"could not open {port}: {last}")

    def open_sim(self, binary: str) -> None:
        self._proc = subprocess.Popen([binary, "--serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.DEVNULL, bufsize=0)
        self._write = lambda b: (self._proc.stdin.write(b), self._proc.stdin.flush())
        threading.Thread(target=self._read_pipe, daemon=True).start()

    def close(self) -> None:
        self._closed = True
        if getattr(self, "_ser", None):
            self._ser.close()
        if getattr(self, "_proc", None):
            try:
                self._write(b"#quit\n")
            except Exception:  # noqa: BLE001
                pass
            self._proc.terminate()

    # ---- reading
    def _deliver(self, line: str) -> None:
        line = line.strip()
        if not line:
            return
        data = None
        if line.startswith("{"):
            try:
                data = json.loads(line)
            except ValueError:
                data = None
        ev = Event(time.monotonic(), data, line)
        self.history.append(ev)
        self.events.put(ev)
        if self.on_event:
            self.on_event(ev)

    def _read_serial(self) -> None:
        buf = b""
        while not self._closed:
            try:
                chunk = self._ser.read(512)
            except Exception:  # noqa: BLE001 - port vanished (reset / unplug)
                self._deliver('{"evt":"_link","state":"lost"}')
                return
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self._deliver(line.decode("utf-8", "replace"))

    def _read_pipe(self) -> None:
        for line in iter(self._proc.stdout.readline, b""):
            self._deliver(line.decode("utf-8", "replace"))

    # ---- commands
    def send(self, cmd: str, **params) -> int:
        cid = self._next_id
        self._next_id += 1
        msg = {"v": 1, "id": cid, "cmd": cmd, **params}
        self._write((json.dumps(msg, ensure_ascii=False) + "\n").encode("utf-8"))
        return cid

    def send_raw(self, line: str) -> None:
        self._write((line + "\n").encode("utf-8"))

    def wait_for(self, pred: Callable[[dict], bool], timeout: float) -> dict | None:
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            try:
                ev = self.events.get(timeout=left)
            except queue.Empty:
                return None
            if ev.data is not None and pred(ev.data):
                return ev.data

    def request(self, cmd: str, timeout: float = 3.0, **params) -> dict | None:
        """Send a command and return its ack (or None on timeout)."""
        cid = self.send(cmd, **params)
        return self.wait_for(lambda d: d.get("evt") == "ack" and d.get("id") == cid, timeout)

    def drain(self) -> None:
        while not self.events.empty():
            self.events.get_nowait()


def eprint(*a) -> None:
    print(*a, file=sys.stderr)
