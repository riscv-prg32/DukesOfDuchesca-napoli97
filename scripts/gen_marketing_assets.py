#!/usr/bin/env python3
"""Generate assets/icon.png and assets/screenshot.png.

These are hand-composed mockups (matching the style of the upstream PRG32
cartridges' own render_assets.py-style promo art), not literal emulator
captures -- this sandbox has no ESP-IDF/RISC-V toolchain to actually run the
cartridge and grab a frame. They reuse the exact same vehicle silhouettes
drawn in gen_assets.py and the exact same tile color table used by
game.c's define_tiles(), so they represent the real in-game look faithfully.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_assets import draw_car, draw_scooter, draw_police, draw_villa

from PIL import Image, ImageDraw

OUT = Path(__file__).resolve().parents[1] / "assets"
OUT.mkdir(exist_ok=True)

TILE = 8
ASPHALT = (58, 58, 62)
LINE = (230, 210, 40)
BRICK_BG = (218, 177, 145)
BRICK_FG = (140, 84, 56)
PLAZA_BG = (255, 210, 130)
GULF = (26, 62, 112)


def paste_rgba(base, img, x, y):
    base.paste(img, (x, y), img if img.mode == "RGBA" else None)


def draw_city_patch(w, h, offset=0):
    im = Image.new("RGB", (w, h), BRICK_BG)
    d = ImageDraw.Draw(im)
    for by in range(0, h, TILE):
        for bx in range(0, w, TILE):
            d.rectangle((bx, by, bx + TILE - 1, by + TILE - 1), fill=BRICK_BG)
            d.line((bx, by, bx + TILE - 1, by), fill=BRICK_FG)
    # decumano (wide horizontal road)
    ry = h // 2 - TILE
    d.rectangle((0, ry, w, ry + 2 * TILE - 1), fill=ASPHALT)
    for x in range(-offset % 16, w, 16):
        d.rectangle((x, ry + TILE - 1, x + 8, ry + TILE), fill=LINE)
    # a couple of cardini (narrow vertical alleys)
    for cx in range(20, w, 48):
        d.rectangle((cx, 0, cx + TILE - 1, h), fill=ASPHALT)
    # a piazza patch
    d.rectangle((w - 70, 10, w - 10, 60), fill=PLAZA_BG)
    return im


def make_screenshot():
    w, h = 320, 200
    im = draw_city_patch(w, h)
    d = ImageDraw.Draw(im)

    car = draw_car(35).convert("RGBA")
    car = car.rotate(0, expand=False)
    mask = Image.new("L", car.size, 0)
    px = car.load()
    for yy in range(car.size[1]):
        for xx in range(car.size[0]):
            r, g, b, a = px[xx, yy]
            mask.putpixel((xx, yy), 0 if (r, g, b) == (0, 0, 0) else 255)
    car.putalpha(mask)
    paste_rgba(im, car, 150, 92)

    cop = draw_police(200).convert("RGBA")
    mask = Image.new("L", cop.size, 0)
    px = cop.load()
    for yy in range(cop.size[1]):
        for xx in range(cop.size[0]):
            r, g, b, a = px[xx, yy]
            mask.putpixel((xx, yy), 0 if (r, g, b) == (0, 0, 0) else 255)
    cop.putalpha(mask)
    paste_rgba(im, cop, 60, 60)

    scoot = draw_scooter(160).convert("RGBA")
    mask = Image.new("L", scoot.size, 0)
    px = scoot.load()
    for yy in range(scoot.size[1]):
        for xx in range(scoot.size[0]):
            r, g, b, a = px[xx, yy]
            mask.putpixel((xx, yy), 0 if (r, g, b) == (0, 0, 0) else 255)
    scoot.putalpha(mask)
    paste_rgba(im, scoot, 230, 130)

    # HUD mockup
    d.rectangle((6, 6, 86, 14), fill=(20, 20, 24))
    d.rectangle((6, 6, 60, 14), fill=(60, 200, 90))
    d.text((6, 16), "GAS", fill=(255, 255, 255))
    d.text((96, 16), "ITEMS 3/8", fill=(255, 230, 60))
    d.text((236, 6), "TROUBLE", fill=(255, 255, 255))
    for i in range(3):
        d.rectangle((236 + i * 10, 16, 244 + i * 10, 24), fill=(90, 60, 60))

    im.save(OUT / "screenshot.png")


def make_icon():
    w = h = 256
    im = Image.new("RGB", (w, h), GULF)
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / h
        col = tuple(int(GULF[i] * (1 - t) + BRICK_BG[i] * 0.3 * t + 10) for i in range(3))
        d.line((0, y, w, y), fill=col)
    # Vesuvio silhouette
    d.polygon([(40, 170), (110, 90), (140, 120), (180, 70), (230, 170)], fill=(30, 40, 60))
    d.rectangle((0, 170, w, h), fill=(70, 46, 34))
    for i in range(6):
        d.rectangle((10 + i * 42, 178, 40 + i * 42, h), fill=(150, 96, 66))

    car = draw_car(0).resize((160, 160), Image.NEAREST).convert("RGBA")
    mask = Image.new("L", car.size, 0)
    px = car.load()
    for yy in range(car.size[1]):
        for xx in range(car.size[0]):
            r, g, b, a = px[xx, yy]
            mask.putpixel((xx, yy), 0 if (r, g, b) == (0, 0, 0) else 255)
    car.putalpha(mask)
    paste_rgba(im, car, (w - 160) // 2, 60)

    d.text((96, 8), "'97", fill=(255, 220, 90))
    im.save(OUT / "icon.png")


if __name__ == "__main__":
    make_icon()
    make_screenshot()
    print("wrote", OUT / "icon.png", "and", OUT / "screenshot.png")
