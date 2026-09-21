#!/usr/bin/env python3
"""Generate assets_generated.h for DukesOfDuchesca-napoli97.

This is a dev-time tool (like cartridges/poing/scripts/render_assets.py in
the upstream PRG32 repo): it rasterizes small vehicle silhouettes at 8
headings and one landmark image, quantizes them to fixed palettes, and
emits packed prg32_indexed_sprite_t-compatible C arrays. Its output
(assets_generated.h) is committed to the repo; the cartridge build does not
re-run this script.

Packing matches components/prg32/prg32_sprite.c's non-planar decoder
exactly: pixels are row-major, two 4-bit (or one 8-bit) palette indices per
byte, packed continuously across the whole width*height frame with no
per-row padding, most significant nibble first.
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).resolve().parents[1] / "assets_generated.h"

HEADINGS = 8  # N, NE, E, SE, S, SW, W, NW (index 0 = up, clockwise)


def rotated_polygon(points, angle_deg, cx, cy):
    a = math.radians(angle_deg)
    ca, sa = math.cos(a), math.sin(a)
    out = []
    for x, y in points:
        rx = x * ca - y * sa
        ry = x * sa + y * ca
        out.append((cx + rx, cy + ry))
    return out


def quantize_to_palette(im, palette_rgb):
    """Map each pixel to the nearest palette index (small palettes only)."""
    px = im.load()
    w, h = im.size
    indices = [0] * (w * h)
    cache = {}
    for y in range(h):
        for x in range(w):
            c = px[x, y]
            idx = cache.get(c)
            if idx is None:
                best, best_d = 0, None
                for i, pc in enumerate(palette_rgb):
                    d = sum((a - b) * (a - b) for a, b in zip(c, pc))
                    if best_d is None or d < best_d:
                        best, best_d = i, d
                idx = best
                cache[c] = idx
            indices[y * w + x] = idx
    return indices


def pack_4bpp(indices):
    out = bytearray()
    it = iter(indices)
    for hi in it:
        lo = next(it, 0)
        out.append(((hi & 0xF) << 4) | (lo & 0xF))
    return bytes(out)


def pack_8bpp(indices):
    return bytes(i & 0xFF for i in indices)


def emit_array(lines, c_type, name, data):
    lines.append(f"static const {c_type} {name}[{len(data)}] = {{")
    row = []
    for i, v in enumerate(data):
        row.append((f"0x{v:02x}" if c_type == "uint8_t" else f"0x{v:04x}") + ",")
        if len(row) == 16:
            lines.append("    " + " ".join(row))
            row = []
    if row:
        lines.append("    " + " ".join(row))
    lines.append("};")


CAR_PALETTE = [
    (0, 0, 0),        # 0 transparent (key)
    (250, 250, 245),  # 1 white pimped body
    (40, 40, 46),      # 2 tyres / window glass dark
    (170, 20, 30),     # 3 red go-fast stripe / tail light
    (255, 214, 60),    # 4 headlight / chrome trim
]

SCOOTER_PALETTE = [
    (0, 0, 0),        # 0 transparent
    (210, 60, 40),     # 1 scooter body (gang red)
    (30, 30, 30),      # 2 wheels / rider jacket
    (245, 210, 90),    # 3 helmet
]

POLICE_PALETTE = [
    (0, 0, 0),        # 0 transparent
    (240, 240, 240),  # 1 white body
    (25, 60, 150),     # 2 blue livery
    (210, 30, 30),     # 3 light bar red
    (30, 90, 220),     # 4 light bar blue
]

CANVAS = 20
CX = CY = CANVAS / 2.0


def draw_car(angle):
    im = Image.new("RGB", (CANVAS, CANVAS), CAR_PALETTE[0])
    d = ImageDraw.Draw(im)
    body = [(-3.5, -7), (3.5, -7), (4.5, 6), (-4.5, 6)]
    d.polygon(rotated_polygon(body, angle, CX, CY), fill=CAR_PALETTE[1])
    roof = [(-2.5, -3), (2.5, -3), (2.5, 2), (-2.5, 2)]
    d.polygon(rotated_polygon(roof, angle, CX, CY), fill=CAR_PALETTE[2])
    stripe = [(-0.8, -7), (0.8, -7), (0.8, 6), (-0.8, 6)]
    d.polygon(rotated_polygon(stripe, angle, CX, CY), fill=CAR_PALETTE[3])
    for hx in (-3.2, 3.2):
        head = [(hx - 0.7, -7.4), (hx + 0.7, -7.4), (hx + 0.7, -6), (hx - 0.7, -6)]
        d.polygon(rotated_polygon(head, angle, CX, CY), fill=CAR_PALETTE[4])
    return im


def draw_scooter(angle):
    im = Image.new("RGB", (CANVAS, CANVAS), SCOOTER_PALETTE[0])
    d = ImageDraw.Draw(im)
    body = [(-2, -6), (2, -6), (2.5, 5), (-2.5, 5)]
    d.polygon(rotated_polygon(body, angle, CX, CY), fill=SCOOTER_PALETTE[1])
    rider = [(-1.6, -3), (1.6, -3), (1.6, 1.5), (-1.6, 1.5)]
    d.polygon(rotated_polygon(rider, angle, CX, CY), fill=SCOOTER_PALETTE[2])
    helmet = [(-1.4, -6.6), (1.4, -6.6), (1.4, -4.4), (-1.4, -4.4)]
    d.polygon(rotated_polygon(helmet, angle, CX, CY), fill=SCOOTER_PALETTE[3])
    return im


def draw_police(angle):
    im = Image.new("RGB", (CANVAS, CANVAS), POLICE_PALETTE[0])
    d = ImageDraw.Draw(im)
    body = [(-3.8, -7), (3.8, -7), (4.6, 6), (-4.6, 6)]
    d.polygon(rotated_polygon(body, angle, CX, CY), fill=POLICE_PALETTE[1])
    livery = [(-4.6, -1), (4.6, -1), (4.6, 1.4), (-4.6, 1.4)]
    d.polygon(rotated_polygon(livery, angle, CX, CY), fill=POLICE_PALETTE[2])
    bar_l = [(-1.6, -7.6), (0, -7.6), (0, -6.2), (-1.6, -6.2)]
    bar_r = [(0, -7.6), (1.6, -7.6), (1.6, -6.2), (0, -6.2)]
    d.polygon(rotated_polygon(bar_l, angle, CX, CY), fill=POLICE_PALETTE[3])
    d.polygon(rotated_polygon(bar_r, angle, CX, CY), fill=POLICE_PALETTE[4])
    return im


def build_frames(draw_fn, palette, symbol, bpp, lines):
    all_indices = []
    for h in range(HEADINGS):
        angle = h * (360.0 / HEADINGS)
        im = draw_fn(angle)
        idx = quantize_to_palette(im, palette)
        all_indices.extend(idx)
    packed = pack_4bpp(all_indices) if bpp == 4 else pack_8bpp(all_indices)
    emit_array(lines, "uint8_t", f"{symbol}_pixels", list(packed))
    pal16 = [((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3) for r, g, b in palette]
    emit_array(lines, "uint16_t", f"{symbol}_palette", pal16)
    lines.append(f"#define {symbol.upper()}_WIDTH {CANVAS}")
    lines.append(f"#define {symbol.upper()}_HEIGHT {CANVAS}")
    lines.append(f"#define {symbol.upper()}_FRAMES {HEADINGS}")
    lines.append(f"#define {symbol.upper()}_PALETTE_COUNT {len(palette)}")
    lines.append(f"#define {symbol.upper()}_BPP {bpp}")
    lines.append("")


VILLA_W, VILLA_H = 40, 28
VILLA_PALETTE = [
    (10, 10, 30),    # 0 night sky (also used as transparent key)
    (235, 200, 140), # 1 villa wall (warm plaster)
    (150, 60, 50),   # 2 roof tiles (terracotta)
    (255, 225, 90),  # 3 lit windows / string lights
    (60, 150, 70),   # 4 garden greenery
    (230, 60, 140),  # 5 party pink glow
    (250, 250, 250), # 6 gate columns
    (90, 60, 40),    # 7 door / shadow
]


def draw_villa():
    im = Image.new("RGB", (VILLA_W, VILLA_H), VILLA_PALETTE[0])
    d = ImageDraw.Draw(im)
    d.rectangle((2, 22, VILLA_W - 3, VILLA_H - 1), fill=VILLA_PALETTE[4])
    d.rectangle((6, 8, VILLA_W - 7, 22), fill=VILLA_PALETTE[1])
    d.polygon([(4, 8), (VILLA_W - 5, 8), (VILLA_W - 11, 2), (10, 2)],
              fill=VILLA_PALETTE[2])
    for wx in (10, 17, VILLA_W - 24, VILLA_W - 17):
        d.rectangle((wx, 12, wx + 4, 17), fill=VILLA_PALETTE[3])
    d.rectangle((VILLA_W // 2 - 3, 15, VILLA_W // 2 + 3, 22), fill=VILLA_PALETTE[7])
    d.rectangle((7, 20, 9, 27), fill=VILLA_PALETTE[6])
    d.rectangle((VILLA_W - 10, 20, VILLA_W - 8, 27), fill=VILLA_PALETTE[6])
    for i in range(6):
        x = 4 + i * 6
        d.point((x, 4), fill=VILLA_PALETTE[5])
        d.point((x + 2, 5), fill=VILLA_PALETTE[3])
    return im


def main():
    lines = [
        "/* AUTO-GENERATED by scripts/gen_assets.py -- do not hand-edit. */",
        "#ifndef DUKES_ASSETS_GENERATED_H",
        "#define DUKES_ASSETS_GENERATED_H",
        "#include <stdint.h>",
        "",
    ]
    build_frames(draw_car, CAR_PALETTE, "duke_car", 4, lines)
    build_frames(draw_scooter, SCOOTER_PALETTE, "duke_scooter", 4, lines)
    build_frames(draw_police, POLICE_PALETTE, "duke_police", 4, lines)

    villa_idx = quantize_to_palette(draw_villa(), VILLA_PALETTE)
    emit_array(lines, "uint8_t", "duke_villa_pixels", list(pack_8bpp(villa_idx)))
    pal16 = [((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3) for r, g, b in VILLA_PALETTE]
    emit_array(lines, "uint16_t", "duke_villa_palette", pal16)
    lines.append(f"#define DUKE_VILLA_WIDTH {VILLA_W}")
    lines.append(f"#define DUKE_VILLA_HEIGHT {VILLA_H}")
    lines.append(f"#define DUKE_VILLA_PALETTE_COUNT {len(VILLA_PALETTE)}")
    lines.append("")
    lines.append("#endif")

    OUT.write_text("\n".join(lines) + "\n")
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
