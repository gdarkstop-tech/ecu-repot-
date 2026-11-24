"""Turn simulator frame dumps into reviewable images.

Input : DIR/<scene>.frames  (raw 1 bpp 128x64 frames, row-major, MSB = leftmost pixel)
Output: OUT/<scene>.gif     animated, 4x scale, ~30 fps
        OUT/oled_gallery.png  one representative frame per scene, labelled

These images are rendered by the firmware's own drawing code (via the U8g2 C library on the
host). They show what the SH1106 will display; they are not photos of the physical OLED.
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw

W, H, SCALE = 128, 64, 4
FRAME_BYTES = W * H // 8
ON = (225, 240, 255)
OFF = (6, 8, 12)
GAP = (14, 17, 22)
ON_GAP = (176, 190, 204)


def load_frames(path: Path) -> list[bytes]:
    data = path.read_bytes()
    return [data[i : i + FRAME_BYTES] for i in range(0, len(data) - FRAME_BYTES + 1, FRAME_BYTES)]


def frame_to_image(frame: bytes, scale: int = SCALE) -> Image.Image:
    mono = Image.frombytes("1", (W, H), frame)
    img = Image.new("RGB", (W * scale, H * scale), GAP)
    px = mono.load()
    draw = ImageDraw.Draw(img)
    for y in range(H):
        for x in range(W):
            on = px[x, y]
            # A faint gap between pixels, like the real panel (lighter on a lit area so the light,
            # BMO-style screen does not look like a dark mesh).
            draw.rectangle([x * scale, y * scale, x * scale + scale - 1, y * scale + scale - 1],
                           fill=ON_GAP if on else GAP)
            draw.rectangle([x * scale, y * scale, x * scale + scale - 2, y * scale + scale - 2],
                           fill=ON if on else OFF)
    return img


# The BMO-inspired faces, in the order of the design list (docs/FACE_DESIGN.md).
FACE_SCENES = ["idle", "blink", "happy", "excited", "listening", "thinking", "speaking", "sleepy", "sleeping",
               "curious", "confused", "sad", "worried", "playful", "proud", "surprised", "love", "starstruck",
               "dizzy", "laughing", "crying", "annoyed", "shy", "scared", "mischievous", "bored", "cool", "muted",
               "offline", "error", "boot"]
CW, CH = 128 * 3, 64 * 3  # colour previews from the simulator (BMO palette, 3x)


def colour_frames(path: Path) -> list[Image.Image]:
    data = path.read_bytes()
    n = CW * CH * 3
    return [Image.frombytes("RGB", (CW, CH), data[i:i + n]) for i in range(0, len(data) - n + 1, n)]


def colour_media(src: Path, out: Path) -> None:
    """faces_color.png + <name>_color.gif: the same faces in BMO's colours (what a colour display shows)."""
    tiles = []
    for name in FACE_SCENES:
        path = src / f"{name}.rgb"
        if not path.exists():
            continue
        frames = colour_frames(path)
        if not frames:
            continue
        frames[0].save(out / f"{name}_color.gif", save_all=True, append_images=frames[1:], duration=66, loop=0,
                       optimize=True)
        pick = frames[min(len(frames) - 1, len(frames) // 2)]
        if name == "speaking":
            pick = max(frames, key=lambda f: sum(f.convert("L").histogram()[:120]))  # widest mouth
        tiles.append((name, pick))
    cols, label_h = 4, 22
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (CW + 12) + 12, rows * (CH + label_h + 12) + 12), (30, 33, 40))
    d = ImageDraw.Draw(sheet)
    for i, (name, tile) in enumerate(tiles):
        cx = 12 + (i % cols) * (CW + 12)
        cy = 12 + (i // cols) * (CH + label_h + 12)
        sheet.paste(tile, (cx, cy + label_h))
        d.text((cx + 2, cy + 4), name, fill=(200, 210, 225))
    sheet.save(out / "faces_color.png")


def write_sheet(tiles, path, order=None, cols=4) -> None:
    if order:
        tiles = sorted(tiles, key=lambda nt: order.index(nt[0]))
    tw, th, label_h = W * 3, H * 3, 22
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (tw + 12) + 12, rows * (th + label_h + 12) + 12), (30, 33, 40))
    d = ImageDraw.Draw(sheet)
    for i, (name, tile) in enumerate(tiles):
        cx = 12 + (i % cols) * (tw + 12)
        cy = 12 + (i // cols) * (th + label_h + 12)
        sheet.paste(tile, (cx, cy + label_h))
        d.text((cx + 2, cy + 4), name, fill=(200, 210, 225))
    sheet.save(path)


def main() -> int:
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    scenes = sorted(src.glob("*.frames"), key=lambda p: p.stat().st_mtime)
    tiles: list[tuple[str, Image.Image]] = []
    for path in scenes:
        frames = load_frames(path)
        if not frames:
            continue
        name = path.stem
        # GIF: every 2nd frame (~15 fps) keeps files small while staying smooth.
        gif_frames = [frame_to_image(f, 3) for f in frames[::2]]
        gif_frames[0].save(
            out / f"{name}.gif", save_all=True, append_images=gif_frames[1:], duration=66, loop=0, optimize=True
        )
        pick = {"boot": frames[int(len(frames) * 0.45)], "blink": frames[min(2, len(frames) - 1)],
                "reading_en": frames[min(30, len(frames) - 1)], "reading_ar": frames[min(30, len(frames) - 1)]}.get(name, frames[len(frames) // 2])
        if name == "speaking":  # show the talking mouth at its widest (most dark pixels on a lit screen)
            pick = max(frames, key=lambda f: sum(8 - bin(b).count("1") for b in f))
        tiles.append((name, frame_to_image(pick, 3)))

    cols = 4
    tw, th = W * 3, H * 3
    label_h = 22
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (tw + 12) + 12, rows * (th + label_h + 12) + 12), (30, 33, 40))
    d = ImageDraw.Draw(sheet)
    for i, (name, tile) in enumerate(tiles):
        cx = 12 + (i % cols) * (tw + 12)
        cy = 12 + (i // cols) * (th + label_h + 12)
        sheet.paste(tile, (cx, cy + label_h))
        d.text((cx + 2, cy + 4), name, fill=(200, 210, 225))
    sheet.save(out / "oled_gallery.png")
    write_sheet([(n, im) for n, im in tiles if n in FACE_SCENES], out / "faces.png",
                order=FACE_SCENES)
    colour_media(src, out)
    print(f"wrote {len(tiles)} scenes to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
