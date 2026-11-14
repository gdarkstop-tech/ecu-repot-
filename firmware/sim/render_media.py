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
            colour = ON if px[x, y] else OFF
            draw.rectangle(
                [x * scale, y * scale, x * scale + scale - 2, y * scale + scale - 2], fill=colour
            )
    return img


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
    print(f"wrote {len(tiles)} scenes to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
