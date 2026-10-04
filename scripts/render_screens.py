#!/usr/bin/env python3
"""Render the cartridge's real screens on the host and make the Store media.

Runs tests/host_harness.c, which plays the game through a model of the
ESP32-C6's indexed framebuffer (checked pixel for pixel against the QEMU
model on every frame), and converts the frames it dumps:

  release-artifacts/screens/*.png   every screen
  release-artifacts/contact-sheet.png
  screenshot.png                    the Store screenshot (320x200, palette)
  icon.png                          the Store icon (96x96, palette)

    PRG32_REPO=/path/to/PRG32 python3 scripts/render_screens.py
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_assets as art  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
PRG32 = Path(os.environ.get("PRG32_REPO", ROOT.parent / "PRG32"))
OUT = ROOT / "release-artifacts"


def vehicle(shapes, palette, heading, scale):
    indices = art.render(shapes, heading * 45.0)
    im = Image.new("RGBA", (art.CANVAS, art.CANVAS), (0, 0, 0, 0))
    im.putdata([(0, 0, 0, 0) if i == 0 else palette[i] + (255,) for i in indices])
    return im.resize((art.CANVAS * scale, art.CANVAS * scale), Image.NEAREST)


def make_icon():
    """The title's sunset over the gulf, with the Fiat and its pursuers."""
    size = 96
    im = Image.new("RGB", (size, size))
    bands = [(24, 24, 112), (72, 40, 120), (136, 56, 96), (208, 72, 72), (248, 112, 48), (248, 172, 32)]
    for i, colour in enumerate(bands):
        im.paste(colour, (0, i * 9, size, i * 9 + 9))
    for i in range(14):   # the Vesuvio
        im.paste((72, 68, 64), (40 - i * 2, 28 + i * 2, 62 + i * 2, 30 + i * 2))
    im.paste((255, 236, 96), (70, 36, 80, 46))
    im.paste((16, 84, 168), (0, 54, size, 66))
    im.paste((248, 248, 248), (0, 66, size, 68))
    im.paste((56, 60, 64), (0, 68, size, size))
    for x in range(4, size, 24):
        im.paste((236, 228, 80), (x, 81, x + 12, 83))
    for shapes, palette, x, y in ((art.police_shapes, art.POLICE_PALETTE, -6, 62),
                                  (art.scooter_shapes, art.SCOOTER_PALETTE, 16, 72),
                                  (art.car_shapes, art.CAR_PALETTE, 50, 60)):
        sprite = vehicle(shapes, palette, 2, 2)
        im.paste(sprite, (x, y), sprite)
    return im.quantize(colors=32, dither=Image.Dither.NONE)


def main():
    inc = [f"-I{PRG32 / 'components/prg32/include'}", f"-I{PRG32 / 'components/prg32_audio/include'}", f"-I{ROOT}"]
    screens = OUT / "screens"
    screens.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "harness"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-DDUKE_HOST_TEST", *inc,
                        str(ROOT / "game.c"), str(ROOT / "tests/prg32_stub.c"),
                        str(ROOT / "tests/host_harness.c"), "-o", str(exe)], check=True)
        subprocess.run([str(exe), tmp], check=True, stdout=subprocess.DEVNULL)
        frames = sorted(Path(tmp).glob("*.ppm"))
        images = []
        for frame in frames:
            im = Image.open(frame).convert("RGB")
            im.quantize(colors=128, dither=Image.Dither.NONE).save(screens / (frame.stem + ".png"), optimize=True)
            images.append((frame.stem, im))
    sheet = Image.new("RGB", (640, 200 * ((len(images) + 1) // 2)))
    for i, (_, im) in enumerate(images):
        sheet.paste(im, ((i % 2) * 320, (i // 2) * 200))
    sheet.quantize(colors=128, dither=Image.Dither.NONE).save(OUT / "contact-sheet.png", optimize=True)
    shot = dict(images)["03-drive"]
    shot.quantize(colors=48, dither=Image.Dither.NONE).save(ROOT / "screenshot.png", optimize=True)
    make_icon().save(ROOT / "icon.png", optimize=True)
    print(f"wrote {len(images)} screens, contact-sheet.png, screenshot.png, icon.png")


if __name__ == "__main__":
    main()
