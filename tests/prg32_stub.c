/* Host-only stand-in for the PRG32 firmware's public API, implementing just
 * the entry points game.c calls. Declarations come from the real prg32.h
 * (included via -I into the upstream PRG32 checkout), so any ABI drift in
 * game.c's calls is still caught at compile time; only the *bodies* here are
 * fakes. prg32_playfield_put/get and prg32_sprite_hitbox reproduce the real
 * firmware's logic (components/prg32/prg32_tile.c, prg32_sprite.c) closely
 * enough to exercise game.c's streaming and pickup code paths honestly.
 */
#include "prg32.h"
#include <assert.h>

uint32_t g_test_input;
static uint32_t s_rng_state = 0x1234567u;

uint32_t prg32_input_read(void) { return g_test_input; }

uint32_t prg32_random_number(uint32_t min, uint32_t max) {
    if (max <= min) return min;
    s_rng_state = s_rng_state * 1103515245u + 12345u;
    return min + (s_rng_state >> 8) % (max - min + 1u);
}

void prg32_buzzer_tone(uint32_t hz, uint32_t ms, uint16_t duty) {
    (void)hz; (void)ms; (void)duty;
}

void prg32_gfx_clear(uint16_t color) { (void)color; }
void prg32_gfx_rect(int x, int y, int w, int h, uint16_t color) {
    (void)x; (void)y; (void)w; (void)h; (void)color;
}
void prg32_gfx_text8(int x, int y, const char *s, uint16_t fg, uint16_t bg) {
    (void)x; (void)y; (void)s; (void)fg; (void)bg;
}

void prg32_tile_define(uint8_t id, const uint8_t *bitmap8x8, uint16_t fg, uint16_t bg) {
    (void)id; (void)bitmap8x8; (void)fg; (void)bg;
}

static uint8_t s_playfield[PRG32_PLAYFIELD_LAYERS][PRG32_PLAYFIELD_ROWS][PRG32_PLAYFIELD_COLS];
static int s_camera_x, s_camera_y;

void prg32_playfield_clear(uint8_t layer, uint8_t tile_id) {
    assert(layer < PRG32_PLAYFIELD_LAYERS);
    for (int y = 0; y < PRG32_PLAYFIELD_ROWS; ++y)
        for (int x = 0; x < PRG32_PLAYFIELD_COLS; ++x)
            s_playfield[layer][y][x] = tile_id;
}

void prg32_playfield_put(uint8_t layer, uint8_t tx, uint8_t ty, uint8_t id) {
    assert(layer < PRG32_PLAYFIELD_LAYERS);
    assert(tx < PRG32_PLAYFIELD_COLS);
    assert(ty < PRG32_PLAYFIELD_ROWS);
    s_playfield[layer][ty][tx] = id;
}

uint8_t prg32_playfield_get(uint8_t layer, uint8_t tx, uint8_t ty) {
    if (layer >= PRG32_PLAYFIELD_LAYERS || tx >= PRG32_PLAYFIELD_COLS ||
        ty >= PRG32_PLAYFIELD_ROWS)
        return 0;
    return s_playfield[layer][ty][tx];
}

void prg32_playfield_parallax(uint8_t layer, int x_q8, int y_q8) {
    (void)layer; (void)x_q8; (void)y_q8;
}

void prg32_playfield_camera(int x, int y) { s_camera_x = x; s_camera_y = y; }
int prg32_playfield_camera_x(void) { return s_camera_x; }
int prg32_playfield_camera_y(void) { return s_camera_y; }
void prg32_playfield_draw_dual(void) { /* rendering only, no-op on host */ }

int prg32_sprite_hitbox(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    if (aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0) return 0;
    if (ax + aw <= bx || bx + bw <= ax) return 0;
    if (ay + ah <= by || by + bh <= ay) return 0;
    return 1;
}

void prg32_sprite_draw_8x8(int x, int y, const uint8_t *bits, uint16_t fg, uint16_t bg) {
    (void)x; (void)y; (void)bits; (void)fg; (void)bg;
}

void prg32_sprite_draw_indexed(int x, int y, const prg32_indexed_sprite_t *sprite,
                               uint32_t frame) {
    (void)x; (void)y;
    assert(sprite != 0);
    assert(frame < sprite->frame_count);
}
