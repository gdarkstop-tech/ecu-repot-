"""Robot device credentials.

Each robot gets its own random token. The gateway stores only the SHA-256 of each token, so
the registry file is not a secret, and a lost robot is cut off by removing/disabling its entry.
A device token grants access to THIS gateway only — it is not a cloud AI API key.

Create a token:   python -m robot_gateway.devices add robot-1 --file devices.json
(The token is printed once. Provision it to the robot over USB: see the README.)
"""
from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import secrets
import threading
from dataclasses import dataclass
from pathlib import Path


def hash_token(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


@dataclass(frozen=True)
class Device:
    id: str
    enabled: bool = True


class DeviceRegistry:
    def __init__(self, path: str | None = None, inline: str = "") -> None:
        self._lock = threading.Lock()
        self._by_hash: dict[str, Device] = {}
        if path and Path(path).is_file():
            data = json.loads(Path(path).read_text(encoding="utf-8"))
            for d in data.get("devices", []):
                self._by_hash[d["token_sha256"].lower()] = Device(d["id"], bool(d.get("enabled", True)))
        for item in filter(None, (x.strip() for x in inline.split(","))):
            dev_id, _, digest = item.partition(":")
            if len(digest) == 64:
                self._by_hash[digest.lower()] = Device(dev_id)

    def __len__(self) -> int:
        return len(self._by_hash)

    def authenticate(self, authorization: str | None) -> Device | None:
        if not authorization or not authorization.startswith("Bearer "):
            return None
        token = authorization[7:].strip()
        if not (16 <= len(token) <= 128):
            return None
        digest = hash_token(token)
        with self._lock:
            for known, dev in self._by_hash.items():
                if hmac.compare_digest(known, digest):  # constant-time per comparison
                    return dev if dev.enabled else None
        return None


def _cli() -> None:
    ap = argparse.ArgumentParser(description="Manage robot device tokens")
    sub = ap.add_subparsers(dest="cmd", required=True)
    add = sub.add_parser("add", help="create a device and print its token once")
    add.add_argument("device_id")
    add.add_argument("--file", default="devices.json")
    dis = sub.add_parser("disable", help="revoke a device")
    dis.add_argument("device_id")
    dis.add_argument("--file", default="devices.json")
    args = ap.parse_args()

    path = Path(args.file)
    data = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {"devices": []}
    if args.cmd == "add":
        if any(d["id"] == args.device_id for d in data["devices"]):
            raise SystemExit(f"device {args.device_id!r} already exists")
        token = "rbt_" + secrets.token_urlsafe(32)
        digest = hash_token(token)
        data["devices"].append({"id": args.device_id, "token_sha256": digest, "enabled": True})
        path.write_text(json.dumps(data, indent=2), encoding="utf-8")
        print(f"Device {args.device_id} added to {path}.")
        print(f"Token (shown ONCE, store it only on the robot): {token}")
        print(f"Or set on the gateway host instead of the file: DEVICE_TOKEN_HASHES={args.device_id}:{digest}")
    else:
        for d in data["devices"]:
            if d["id"] == args.device_id:
                d["enabled"] = False
        path.write_text(json.dumps(data, indent=2), encoding="utf-8")
        print(f"Device {args.device_id} disabled.")


if __name__ == "__main__":
    _cli()
