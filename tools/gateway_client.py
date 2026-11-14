"""Talk to the AI gateway exactly like the robot does — to test a deployment from the laptop.

    python tools/gateway_client.py --url https://your-gateway --health
    python tools/gateway_client.py --url https://your-gateway --token rbt_... --no-speaker --text "what is 7 plus 5?"
    python tools/gateway_client.py --url ... --token ... --no-speaker --text "let's do math" --text "12"
    python tools/gateway_client.py --url ... --token ... --wav question.wav     (16-bit mono WAV)
    python tools/gateway_client.py --url ... --token ... --tone 1.5            (synthetic 1.5 s sound)

Several --text turns form one conversation: like the robot, each request sends back the
context token ("ctx") of the previous reply, so the gateway remembers the conversation even
on a serverless host (Vercel). --no-speaker asks for text-only answers, like a robot without a
speaker. Otherwise the reply audio is saved as reply.wav so you can hear what the robot would play.
The token can also come from the ROBOT_TOKEN environment variable (avoids shell history).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import struct
import sys
import time
import wave

import httpx


def show(body: dict) -> None:
    shown = dict(body)
    if shown.get("ctx"):
        shown["ctx"] = f"<sealed conversation context, {len(body['ctx'])} characters>"
    print(json.dumps(shown, indent=2, ensure_ascii=False))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", required=True)
    ap.add_argument("--token", default=os.environ.get("ROBOT_TOKEN", ""))
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--health", action="store_true", help="show /healthz (no token needed)")
    g.add_argument("--text", action="append", help="one turn; repeat for a conversation")
    g.add_argument("--wav")
    g.add_argument("--tone", type=float)
    ap.add_argument("--lang", default="auto")
    ap.add_argument("--age", default="6")
    ap.add_argument("--no-speaker", action="store_true", help="text-only answers (robot without a speaker)")
    ap.add_argument("--out", default="reply.wav")
    args = ap.parse_args()

    base = args.url.rstrip("/")
    with httpx.Client(timeout=60) as c:
        if args.health:
            r = c.get(f"{base}/healthz")
            print(f"HTTP {r.status_code}")
            try:
                print(json.dumps(r.json(), indent=2, ensure_ascii=False))
            except ValueError:
                print(r.text[:500])
                return 1
            return 0 if r.status_code == 200 else 1

        if not args.token:
            sys.exit("device token required (--token or ROBOT_TOKEN)")
        headers = {"Authorization": f"Bearer {args.token}", "X-Robot-Lang": args.lang, "X-Robot-Age": args.age}
        if args.no_speaker:
            headers["X-Robot-Output"] = "text"
        ctx = ""
        body: dict = {}
        turns = args.text or [None]
        for i, text in enumerate(turns):
            h = {**headers, **({"X-Robot-Context": ctx} if ctx else {})}
            t0 = time.monotonic()
            if text is not None:
                r = c.post(f"{base}/v1/turns/text", headers=h, json={"text": text, "lang": args.lang})
            else:
                if args.wav:
                    with wave.open(args.wav) as w:
                        if w.getsampwidth() != 2 or w.getnchannels() != 1:
                            sys.exit("WAV must be 16-bit mono")
                        rate, pcm = w.getframerate(), w.readframes(w.getnframes())
                else:
                    rate = 16000
                    pcm = b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 300 * i / rate)))
                                   for i in range(int(args.tone * rate)))
                r = c.post(f"{base}/v1/turns", headers={**h, "Content-Type": f"audio/L16;rate={rate};channels=1"},
                           content=pcm)
            print(f"--- turn {i + 1}: HTTP {r.status_code} in {(time.monotonic() - t0) * 1000:.0f} ms")
            try:
                body = r.json()
            except ValueError:
                print(r.text[:500])
                return 1
            show(body)
            if r.status_code != 200:
                return 1
            ctx = body.get("ctx", ctx)  # an ignored turn has no ctx: keep the previous one, like the robot

        if not body.get("audio"):
            return 0
        a = c.get(base + body["audio"]["path"], headers=headers)
        if a.status_code != 200:
            print(f"audio fetch failed: HTTP {a.status_code}")
            return 1
        with wave.open(args.out, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(body["audio"]["sample_rate"])
            w.writeframes(a.content)
        print(f"reply audio: {len(a.content)} bytes -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
