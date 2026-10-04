#!/usr/bin/env python3
"""Generate assets.h for DukesOfDuchesca-napoli97.

A dev-time tool: it rasterises the three vehicles at 8 headings and the party
villa, and emits packed prg32_indexed_sprite_t-compatible C arrays plus the
1-bpp item icons and compass arrows. Its output (assets.h) is committed; the
cartridge build does not re-run this script.

Vehicles are drawn 4x oversized with hard edges and reduced by majority vote,
which keeps small details (tyres, lights, the roof stripes) crisp at 40x40.
Shapes are in world pixels; the game shows the world at ZOOM screen pixels
per world pixel, so every sprite is rendered ZOOM times larger.

Packing matches components/prg32/prg32_sprite.c's non-planar decoder: pixels
are row-major, packed continuously across the whole frame with no per-row
padding, most significant bits first.
"""
import math
from collections import Counter
from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).resolve().parents[1] / "assets.h"

HEADINGS = 8  # N, NE, E, SE, S, SW, W, NW (index 0 = up, clockwise)
ZOOM = 2      # the game shows the world at two screen pixels per world pixel
CANVAS = 20 * ZOOM
SS = 4        # supersampling factor

# Index 0 is transparent in every palette. The key colour only has to be
# different from the other entries; it is never shown.
KEY = (255, 0, 255)

CAR_PALETTE = [
    KEY,
    (250, 250, 245),  # 1 white body
    (38, 40, 52),     # 2 tyres / glass
    (200, 28, 36),    # 3 red go-faster stripes / tail lights
    (255, 216, 64),   # 4 headlights
    (188, 192, 200),  # 5 body shade
]
SCOOTER_PALETTE = [
    KEY,
    (214, 56, 40),    # 1 scooter body
    (30, 30, 36),     # 2 wheels / handlebar
    (248, 208, 80),   # 3 helmet / headlight
    (60, 96, 176),    # 4 denim jacket
]
POLICE_PALETTE = [
    KEY,
    (240, 242, 246),  # 1 white body
    (28, 64, 160),    # 2 blue livery
    (232, 32, 32),    # 3 light bar, left lamp  (swapped with 4 by the game)
    (40, 112, 248),   # 4 light bar, right lamp
    (36, 38, 50),     # 5 tyres / glass
    (255, 224, 96),   # 6 headlights
]


def rot(points, angle, scale=SS * ZOOM):
    a = math.radians(angle)
    ca, sa = math.cos(a), math.sin(a)
    c = CANVAS * SS / 2.0
    return [(c + (x * ca - y * sa) * scale, c + (x * sa + y * ca) * scale) for x, y in points]


def rect(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


def disc(cx, cy, r, n=12):
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def car_shapes():
    yield 2, rect(-5.3, -5.6, -3.8, -2.6)
    yield 2, rect(3.8, -5.6, 5.3, -2.6)
    yield 2, rect(-5.3, 2.6, -3.8, 5.6)
    yield 2, rect(3.8, 2.6, 5.3, 5.6)
    yield 1, [(-3, -7.2), (3, -7.2), (4.5, -5.4), (4.5, 5.6), (3.4, 7.2), (-3.4, 7.2), (-4.5, 5.6), (-4.5, -5.4)]
    yield 5, rect(3.3, -4.6, 4.5, 5.2)
    yield 3, rect(-1.7, -7.2, -0.5, 6.6)
    yield 3, rect(0.5, -7.2, 1.7, 6.6)
    yield 2, [(-3.1, -3.6), (3.1, -3.6), (3.5, -1.5), (-3.5, -1.5)]
    yield 2, [(-3.1, 3.8), (3.1, 3.8), (2.7, 5.3), (-2.7, 5.3)]
    yield 4, disc(-2.9, -6.2, 1.0)
    yield 4, disc(2.9, -6.2, 1.0)
    yield 3, rect(-4.0, 6.2, -2.4, 7.2)
    yield 3, rect(2.4, 6.2, 4.0, 7.2)


def scooter_shapes():
    yield 2, rect(-1.1, -7.4, 1.1, -4.4)
    yield 2, rect(-1.1, 4.4, 1.1, 7.4)
    yield 1, [(-2.0, -4.8), (2.0, -4.8), (2.5, 4.8), (-2.5, 4.8)]
    yield 2, rect(-3.8, -4.4, 3.8, -3.3)
    yield 3, disc(0, -5.3, 0.9)
    yield 4, disc(0, 1.0, 2.9)
    yield 3, disc(0, -0.8, 1.9)


def police_shapes():
    yield 5, rect(-5.4, -5.8, -3.9, -2.8)
    yield 5, rect(3.9, -5.8, 5.4, -2.8)
    yield 5, rect(-5.4, 2.8, -3.9, 5.8)
    yield 5, rect(3.9, 2.8, 5.4, 5.8)
    yield 1, [(-3.4, -7.6), (3.4, -7.6), (4.6, -6.0), (4.6, 6.4), (3.6, 7.6), (-3.6, 7.6), (-4.6, 6.4), (-4.6, -6.0)]
    yield 2, rect(-4.6, -4.4, -3.2, 5.4)
    yield 2, rect(3.2, -4.4, 4.6, 5.4)
    yield 2, rect(-4.6, -6.0, 4.6, -4.8)
    yield 5, [(-3.0, -3.8), (3.0, -3.8), (3.3, -1.8), (-3.3, -1.8)]
    yield 5, [(-3.0, 4.0), (3.0, 4.0), (2.7, 5.6), (-2.7, 5.6)]
    yield 3, rect(-3.0, -0.6, 0.0, 1.6)
    yield 4, rect(0.0, -0.6, 3.0, 1.6)
    yield 6, disc(-3.0, -6.8, 0.9)
    yield 6, disc(3.0, -6.8, 0.9)


def render(shapes, angle):
    """Return CANVAS*CANVAS palette indices for one heading."""
    big = Image.new("P", (CANVAS * SS, CANVAS * SS), 0)
    d = ImageDraw.Draw(big)
    for index, poly in shapes():
        d.polygon(rot(poly, angle), fill=index)
    px = big.load()
    out = []
    for y in range(CANVAS):
        for x in range(CANVAS):
            votes = Counter(px[x * SS + i, y * SS + j] for i in range(SS) for j in range(SS))
            # A cell is drawn when at least half of it is covered.
            solid = [(n, i) for i, n in votes.items() if i != 0]
            if sum(n for n, _ in solid) * 2 < SS * SS:
                out.append(0)
            else:
                out.append(max(solid)[1])
    return out


def pack(indices, bpp):
    out, acc, bits = bytearray(), 0, 0
    for i in indices:
        acc = (acc << bpp) | i
        bits += bpp
        if bits == 8:
            out.append(acc)
            acc = bits = 0
    if bits:
        out.append(acc << (8 - bits))
    return bytes(out)


def rgb565(c):
    r, g, b = c
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def emit_icons(lines, name, icons):
    lines.append(f"static const uint8_t {name}[{len(icons)}][{len(icons[0])}] = {{")
    for rows in icons:
        lines.append("    {" + ", ".join("0x%02x" % b for b in rows) + "},")
    lines.append("};")


def emit(lines, c_type, name, data):
    lines.append(f"static const {c_type} {name}[{len(data)}] = {{")
    width = 16 if c_type == "uint8_t" else 8
    fmt = "0x%02x," if c_type == "uint8_t" else "0x%04x,"
    for i in range(0, len(data), width):
        lines.append("    " + " ".join(fmt % v for v in data[i:i + width]))
    lines.append("};")


def vehicle(lines, name, shapes, palette):
    indices = []
    for h in range(HEADINGS):
        indices.extend(render(shapes, h * 360.0 / HEADINGS))
    emit(lines, "uint8_t", f"duke_{name}_pixels", pack(indices, 4))
    pal = [0] + [rgb565(c) for c in palette[1:]]
    emit(lines, "uint16_t", f"duke_{name}_palette", pal)
    lines.append(f"#define DUKE_{name.upper()}_COLOURS {len(palette)}")
    lines.append("")


# ---- the party villa: a 48x32 facade standing in its garden ---------------
VILLA_W, VILLA_H = 48, 32
VILLA_PALETTE = [
    KEY,
    (236, 204, 148),  # 1 plaster wall
    (176, 72, 52),    # 2 terracotta roof
    (255, 228, 104),  # 3 lit windows
    (56, 148, 72),    # 4 garden
    (232, 64, 148),   # 5 party pink
    (250, 250, 250),  # 6 columns
    (96, 62, 44),     # 7 door
    (24, 92, 48),     # 8 cypress
    (200, 164, 112),  # 9 wall shade
    (255, 64, 64),    # 10 string light A (cycled by the game)
    (64, 255, 96),    # 11 string light B
    (72, 136, 255),   # 12 string light C
    (64, 176, 224),   # 13 pool
]


def draw_villa():
    im = Image.new("P", (VILLA_W, VILLA_H), 0)
    d = ImageDraw.Draw(im)
    d.rectangle((0, 24, VILLA_W - 1, VILLA_H - 1), fill=4)
    d.rectangle((30, 26, 44, 30), fill=13)
    d.rectangle((8, 9, VILLA_W - 9, 25), fill=1)
    d.rectangle((8, 22, VILLA_W - 9, 25), fill=9)
    d.polygon([(5, 9), (VILLA_W - 6, 9), (VILLA_W - 13, 2), (12, 2)], fill=2)
    for wx in (11, 18, VILLA_W - 23, VILLA_W - 16):
        d.rectangle((wx, 12, wx + 4, 18), fill=3)
    d.rectangle((VILLA_W // 2 - 3, 15, VILLA_W // 2 + 2, 25), fill=7)
    d.rectangle((VILLA_W // 2 - 5, 13, VILLA_W // 2 + 4, 14), fill=5)
    for cx in (8, VILLA_W - 11):
        d.rectangle((cx, 20, cx + 2, 29), fill=6)
    for cx in (1, 4, VILLA_W - 6, VILLA_W - 3):
        d.polygon([(cx, 24), (cx + 1, 11), (cx + 2, 24)], fill=8)
    for i in range(14):        # the string of party lights along the eaves
        d.point((5 + i * 3, 10 + (i & 1)), fill=10 + i % 3)
    return list(im.tobytes())


def doubled_pixels(indices, w):
    """Pixel-double a w-wide index image."""
    out = []
    for y in range(len(indices) // w):
        row = [i for i in indices[y * w:(y + 1) * w] for _ in range(ZOOM)]
        out += row * ZOOM
    return out


def doubled_icon(rows):
    """An 8x8 one-bit icon as 16x16: two bytes per row, 16 rows."""
    out = []
    for b in rows:
        wide = 0
        for bit in range(8):
            if b & (0x80 >> bit):
                wide |= 0xC000 >> (bit * 2)
        out += [wide >> 8, wide & 0xFF] * 2
    return out


# ---- 8x8 one-bit icons: one byte per row, most significant bit leftmost ---
ICONS = [
    ("beer", [0x38, 0x7c, 0x7c, 0x7d, 0x7d, 0x7d, 0x7c, 0x7c], (255, 176, 0)),
    ("wine", [0x18, 0x18, 0x3c, 0x7e, 0x7e, 0x7e, 0x7e, 0x3c], (176, 40, 160)),
    ("sangria", [0xc3, 0x66, 0x3c, 0x18, 0x18, 0x3c, 0x7e, 0x00], (255, 80, 32)),
    ("amplifier", [0xff, 0x81, 0xbd, 0xa5, 0xa5, 0xbd, 0x81, 0xff], (255, 255, 255)),
    ("loudspeaker", [0x03, 0x0f, 0x3f, 0xff, 0xff, 0x3f, 0x0f, 0x03], (255, 216, 0)),
    ("mixer", [0x66, 0x66, 0x00, 0xff, 0x18, 0x18, 0xff, 0x00], (0, 255, 255)),
    ("disco lights", [0x3c, 0x42, 0xa5, 0x99, 0x99, 0xa5, 0x42, 0x3c], (255, 0, 255)),
    ("nice girls", [0x66, 0xff, 0xff, 0x7e, 0x3c, 0x18, 0x00, 0x00], (255, 96, 152)),
]
GAS_ICON = [0x3c, 0x42, 0x5e, 0x42, 0x42, 0x42, 0x7e, 0x00]
RAUTI_ICON = [0x08, 0x10, 0x3c, 0x7e, 0x7e, 0x7e, 0x7e, 0x3c]  # a banger with its fuse


def arrow(angle):
    big = Image.new("L", (8 * SS, 8 * SS), 0)
    d = ImageDraw.Draw(big)
    a = math.radians(angle)
    ca, sa = math.cos(a), math.sin(a)
    pts = [(0, -4), (3.4, 3), (0, 1.2), (-3.4, 3)]
    d.polygon([((4 + x * ca - y * sa) * SS, (4 + x * sa + y * ca) * SS) for x, y in pts], fill=255)
    px = big.resize((8, 8), Image.BOX).load()
    return [sum((1 << (7 - x)) for x in range(8) if px[x, y] >= 128) for y in range(8)]


def main():
    lines = [
        "/* AUTO-GENERATED by scripts/gen_assets.py -- do not hand-edit. */",
        "#ifndef DUKES_ASSETS_H",
        "#define DUKES_ASSETS_H",
        "#include <stdint.h>",
        "",
        f"#define DUKE_VEHICLE_SIZE {CANVAS}",
        f"#define DUKE_VEHICLE_FRAMES {HEADINGS}",
        "",
    ]
    vehicle(lines, "car", car_shapes, CAR_PALETTE)
    vehicle(lines, "scooter", scooter_shapes, SCOOTER_PALETTE)
    vehicle(lines, "police", police_shapes, POLICE_PALETTE)

    emit(lines, "uint8_t", "duke_villa_pixels", pack(doubled_pixels(draw_villa(), VILLA_W), 4))
    emit(lines, "uint16_t", "duke_villa_palette", [0] + [rgb565(c) for c in VILLA_PALETTE[1:]])
    lines += [f"#define DUKE_VILLA_WIDTH {VILLA_W * ZOOM}", f"#define DUKE_VILLA_HEIGHT {VILLA_H * ZOOM}",
              f"#define DUKE_VILLA_COLOURS {len(VILLA_PALETTE)}",
              "#define DUKE_VILLA_LIGHT 10 /* first of three cycled string-light entries */", ""]

    lines.append("/* 8x8 one-bit icons, one per party item, in item order. */")
    emit_icons(lines, "duke_item_icons", [rows for _, rows, _ in ICONS])
    emit(lines, "uint16_t", "duke_item_colours", [rgb565(c) for _, _, c in ICONS])
    emit(lines, "uint8_t", "duke_rauti_icon", RAUTI_ICON)
    lines.append("/* The same icons at 16x16 (two bytes per row) for the zoomed world. */")
    emit_icons(lines, "duke_item_icons16", [doubled_icon(rows) for _, rows, _ in ICONS])
    emit(lines, "uint8_t", "duke_gas_icon16", doubled_icon(GAS_ICON))
    emit(lines, "uint8_t", "duke_rauti_icon16", doubled_icon(RAUTI_ICON))
    lines.append("/* Compass arrows for the 8 headings, N first, clockwise, 16x16. */")
    emit_icons(lines, "duke_arrow_icons16", [doubled_icon(arrow(h * 45)) for h in range(8)])
    lines += ["", "#endif"]
    OUT.write_text("\n".join(lines) + "\n")
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
