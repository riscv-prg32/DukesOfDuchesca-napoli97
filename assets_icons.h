#ifndef DUKES_ASSETS_ICONS_H
#define DUKES_ASSETS_ICONS_H
#include <stdint.h>

/* Hand-authored 8x8 1-bit masks (MSB = leftmost pixel), drawn with
 * prg32_sprite_draw_8x8(x, y, bits, fg, bg). One glyph per party item, plus
 * a gas-pump glyph for the fuel stations. Reused for both the on-map pickup
 * and the HUD checklist row. */

static const uint8_t duke_icon_beer[8] = {
    0x38, 0x7c, 0x7c, 0x7d, 0x7d, 0x7d, 0x7c, 0x7c,
};
static const uint8_t duke_icon_wine[8] = {
    0x18, 0x18, 0x3c, 0x7e, 0x7e, 0x7e, 0x7e, 0x3c,
};
static const uint8_t duke_icon_sangria[8] = {
    0xc3, 0x66, 0x3c, 0x18, 0x18, 0x3c, 0x7e, 0x00,
};
static const uint8_t duke_icon_amplifier[8] = {
    0xff, 0x81, 0xbd, 0xa5, 0xa5, 0xbd, 0x81, 0xff,
};
static const uint8_t duke_icon_loudspeaker[8] = {
    0x03, 0x0f, 0x3f, 0xff, 0xff, 0x3f, 0x0f, 0x03,
};
static const uint8_t duke_icon_mixer[8] = {
    0x66, 0x66, 0x00, 0xff, 0x18, 0x18, 0xff, 0x00,
};
static const uint8_t duke_icon_discolights[8] = {
    0x3c, 0x42, 0xa5, 0x99, 0x99, 0xa5, 0x42, 0x3c,
};
static const uint8_t duke_icon_girls[8] = {
    0x66, 0xff, 0xff, 0x7e, 0x3c, 0x18, 0x00, 0x42,
};

static const uint8_t duke_icon_gas[8] = {
    0x3c, 0x42, 0x5e, 0x42, 0x42, 0x42, 0x7e, 0x00,
};

static const uint8_t *const duke_item_icons[8] = {
    duke_icon_beer,       duke_icon_wine,   duke_icon_sangria,
    duke_icon_amplifier,  duke_icon_loudspeaker,
    duke_icon_mixer,      duke_icon_discolights, duke_icon_girls,
};

static const uint16_t duke_item_colors[8] = {
    0xfd20, /* beer: amber */
    0x780f, /* wine: deep purple */
    0xf9a0, /* sangria: orange-red */
    0x0000, /* amplifier: black on white badge (drawn inverted) */
    0xfea0, /* loudspeaker: gold horn */
    0x07ff, /* mixer: cyan */
    0xf81f, /* disco lights: magenta */
    0xf8d2, /* nice girls: pink heart */
};

#endif
