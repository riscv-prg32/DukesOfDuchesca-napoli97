#!/usr/bin/env python3
"""Render the cartridge's real screens on the host and make the Store media.

Runs tests/host_harness.c, which plays the game through a model of the
ESP32-C6's indexed framebuffer (checked pixel for pixel against the QEMU
model on every frame), and converts the frames it dumps:

  release-artifacts/screens/*.png   every screen
  release-artifacts/map.png         the whole city, points of interest numbered
  release-artifacts/contact-sheet.png
  release-artifacts/points-of-interest.png   every monument, from the grand tour
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


# The points of interest, in the order of citymap.h's CM_AT_* enum.
PLACES = ["Piazza del Plebiscito", "San Francesco di Paola", "Palazzo Reale", "Maschio Angioino",
          "Castel dell'Ovo", "Castel Sant'Elmo", "Stadio San Paolo", "Duomo", "Museo Nazionale",
          "Reggia di Capodimonte", "Villa Doria d'Angri", "Stazione Centrale", "Galleria Umberto I",
          "Centro Direzionale", "Mostra d'Oltremare", "Aeroporto di Capodichino", "Villa Comunale",
          "Molo Beverello", "Piazza Dante", "Piazza Vanvitelli", "Piazza Mercato", "Piazza dei Martiri"]


def make_map(tmp):
    """release-artifacts/map.png: the whole city with its numbered points of interest."""
    exe = Path(tmp) / "map_dump"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", f"-I{ROOT}", str(ROOT / "citymap.c"),
                    str(ROOT / "tests/map_dump.c"), "-o", str(exe)], check=True)
    run = subprocess.run([str(exe)], check=True, capture_output=True)
    ppm = Path(tmp) / "city.map"
    ppm.write_bytes(run.stdout)
    city = Image.open(ppm, formats=["PPM"]).convert("RGB")
    rects = [tuple(int(v) for v in line.split()) for line in run.stderr.decode().splitlines()]
    assert len(rects) == len(PLACES)
    legend_rows = (len(PLACES) + 2) // 3
    im = Image.new("RGB", (city.width, city.height + 8 + legend_rows * 12), (16, 16, 24))
    im.paste(city, (0, 0))
    from PIL import ImageDraw
    d = ImageDraw.Draw(im)
    for n, (x, y, w, h) in enumerate(rects):
        cx, cy = (x + w / 2) * 4, (y + h / 2) * 4
        label = str(n + 1)
        tw = d.textlength(label)
        d.rectangle((cx - tw / 2 - 2, cy - 6, cx + tw / 2 + 1, cy + 5), fill=(0, 0, 0))
        d.text((cx - tw / 2, cy - 6), label, fill=(255, 255, 0))
        d.text((6 + (n % 3) * (city.width // 3), city.height + 6 + (n // 3) * 12),
               f"{n + 1}. {PLACES[n]}", fill=(255, 255, 255))
    im.quantize(colors=48, dither=Image.Dither.NONE).save(OUT / "map.png", optimize=True)


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
        make_map(tmp)
        frames = sorted(Path(tmp).glob("*.ppm"))
        images = []
        for frame in frames:
            im = Image.open(frame).convert("RGB")
            im.quantize(colors=128, dither=Image.Dither.NONE).save(screens / (frame.stem + ".png"), optimize=True)
            images.append((frame.stem, im))
    for name, group, columns in (("contact-sheet.png", [im for n, im in images if not n.startswith("poi-")], 2),
                                 ("points-of-interest.png", [im for n, im in images if n.startswith("poi-")], 3)):
        sheet = Image.new("RGB", (320 * columns, 200 * ((len(group) + columns - 1) // columns)))
        for i, im in enumerate(group):
            sheet.paste(im, ((i % columns) * 320, (i // columns) * 200))
        sheet.quantize(colors=160, dither=Image.Dither.NONE).save(OUT / name, optimize=True)
    shot = dict(images)["03-drive"]
    shot.quantize(colors=48, dither=Image.Dither.NONE).save(ROOT / "screenshot.png", optimize=True)
    make_icon().save(ROOT / "icon.png", optimize=True)
    print(f"wrote {len(images)} screens, contact-sheet.png, screenshot.png, icon.png")


if __name__ == "__main__":
    main()
