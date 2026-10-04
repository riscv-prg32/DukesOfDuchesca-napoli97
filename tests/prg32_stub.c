/* Host-only stand-in for the PRG32 firmware's public API, implementing just
 * the entry points game.c calls. Declarations come from the real prg32.h
 * (included via -I into a PRG32 checkout), so any ABI drift in game.c's
 * calls is still caught at compile time; only the bodies here are fakes.
 *
 * The display is modelled twice, the way the firmware's two back ends work:
 *   - QEMU keeps an RGB565 surface; indexed primitives look the colour up in
 *     the palette when they draw (prg32_display_qemu_rgb.c);
 *   - the ESP32-C6 keeps one byte per pixel; RGB565 colours are mapped to a
 *     named entry or a 6x6x6 cube cell, and the palette is applied when the
 *     frame is presented (prg32_display_ili9341.c, prg32_sprite.c).
 * tests/host_harness.c compares the two after every frame.
 */
#include "prg32.h"
#include "font8.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint32_t g_test_input;
uint32_t g_test_ms;
int g_test_track = -1;
int g_test_tempo;
int g_test_notes;
int g_test_instruments = 11; /* instruments declared by audio.json */
int g_test_tracks = 4;
uint32_t g_test_last_score;

uint16_t g_qemu_fb[PRG32_GAME_H][PRG32_GAME_W];
uint8_t g_c6_fb[PRG32_GAME_H][PRG32_GAME_W];
uint16_t g_palette[256];
static int s_palette_ready;

static void palette_reset(void) {
    static const uint16_t named[8] = {0x0000, 0xffff, 0xf800, 0x07e0, 0x001f, 0xffe0, 0x07ff, 0xf81f};
    for (int i = 0; i < 256; ++i) {
        if (i < 16) g_palette[i] = 0;
        else if (i < 232) {
            unsigned v = (unsigned)i - 16u, r = v / 36u, g = (v / 6u) % 6u, b = v % 6u;
            g_palette[i] = (uint16_t)(((r * 31u / 5u) << 11) | ((g * 63u / 5u) << 5) | (b * 31u / 5u));
        } else {
            unsigned gray = ((unsigned)i - 232u) * 255u / 23u;
            g_palette[i] = (uint16_t)(((gray * 31u / 255u) << 11) | ((gray * 63u / 255u) << 5) |
                                      (gray * 31u / 255u));
        }
    }
    memcpy(g_palette, named, sizeof(named));
    s_palette_ready = 1;
}

static uint8_t index_for(uint16_t color) {
    static const uint16_t named[8] = {0x0000, 0xffff, 0xf800, 0x07e0, 0x001f, 0xffe0, 0x07ff, 0xf81f};
    for (uint8_t i = 0; i < 8; ++i) if (color == named[i]) return i;
    unsigned r = ((color >> 11) & 31u) * 5u / 31u;
    unsigned g = ((color >> 5) & 63u) * 5u / 63u;
    unsigned b = (color & 31u) * 5u / 31u;
    return (uint8_t)(16u + r * 36u + g * 6u + b);
}

static void put_rgb(int x, int y, uint16_t color) {
    if ((unsigned)x >= PRG32_GAME_W || (unsigned)y >= PRG32_GAME_H) return;
    g_qemu_fb[y][x] = color;
    g_c6_fb[y][x] = index_for(color);
}

static void put_index(int x, int y, uint8_t index) {
    if ((unsigned)x >= PRG32_GAME_W || (unsigned)y >= PRG32_GAME_H) return;
    g_qemu_fb[y][x] = g_palette[index];
    g_c6_fb[y][x] = index;
}

uint32_t prg32_input_read(void) { return g_test_input; }
uint32_t prg32_ticks_ms(void) { return g_test_ms; }

void prg32_palette_set(uint8_t index, uint16_t rgb565) {
    if (!s_palette_ready) palette_reset();
    g_palette[index] = rgb565;
}
uint16_t prg32_palette_get(uint8_t index) { return g_palette[index]; }

void prg32_gfx_clear(uint16_t color) {
    if (!s_palette_ready) palette_reset();
    for (int y = 0; y < PRG32_GAME_H; ++y)
        for (int x = 0; x < PRG32_GAME_W; ++x) put_rgb(x, y, color);
}

void prg32_gfx_rect_indexed(int x, int y, int w, int h, uint8_t index) {
    for (int py = y; py < y + h; ++py)
        for (int px = x; px < x + w; ++px) put_index(px, py, index);
}

void prg32_gfx_text8(int x, int y, const char *s, uint16_t fg, uint16_t bg) {
    assert(s);
    for (; *s; ++s, x += 8) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 8; ++col)
                put_rgb(x + col, y + row,
                        (harness_font8[ch - 32][row] & (1u << (7 - col))) ? fg : bg);
    }
}

void prg32_sprite_draw_indexed(int x, int y, const prg32_indexed_sprite_t *s, uint32_t frame) {
    assert(s && s->pixels && s->palette);
    assert(frame < s->frame_count);
    assert(s->bits_per_pixel == 1 || s->bits_per_pixel == 2 || s->bits_per_pixel == 4 ||
           s->bits_per_pixel == 8);
    assert(s->palette_count > 0 && s->palette_count <= (1u << s->bits_per_pixel));
    assert(s->transparent_index >= -1 && s->transparent_index < (int16_t)s->palette_count);
    size_t pixels = (size_t)s->width * s->height;
    size_t frame_bytes = (pixels * s->bits_per_pixel + 7u) / 8u;
    const uint8_t *data = s->pixels + frame * frame_bytes;
    for (int row = 0; row < s->height; ++row) {
        for (int col = 0; col < s->width; ++col) {
            size_t bit = ((size_t)row * s->width + (size_t)col) * s->bits_per_pixel;
            unsigned shift = 8u - s->bits_per_pixel - (unsigned)(bit % 8u);
            unsigned index = (data[bit / 8u] >> shift) & ((1u << s->bits_per_pixel) - 1u);
            if (index >= s->palette_count || (int)index == s->transparent_index) continue;
            put_rgb(x + col, y + row, s->palette[index]);
        }
    }
}

void prg32_audio_play_track(uint16_t track_id) {
    assert(track_id < g_test_tracks);
    g_test_track = track_id;
}
void prg32_audio_set_tempo(uint16_t bpm) {
    assert(bpm >= 30 && bpm <= 300);
    g_test_tempo = bpm;
}
void prg32_audio_note_on_pan(uint8_t channel, uint8_t instrument, uint8_t note, uint8_t volume,
                             int8_t pan) {
    assert(channel >= 5 && channel < 8); /* 0..4 belong to the tracker */
    assert(instrument < g_test_instruments);
    assert(note > 0 && note < 128 && volume > 0);
    assert(pan >= PRG32_AUDIO_PAN_LEFT && pan <= PRG32_AUDIO_PAN_RIGHT);
    g_test_notes++;
}
void prg32_audio_note_off(uint8_t channel) { assert(channel < 8); }

int prg32_score_submit_current_player(const char *game, uint32_t score) {
    assert(game && strlen(game) < 24);
    g_test_last_score = score;
    return 0;
}

/* Write the frame as the ESP32-C6 would show it (binary PPM). */
int stub_dump_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", PRG32_GAME_W, PRG32_GAME_H);
    for (int y = 0; y < PRG32_GAME_H; ++y) {
        for (int x = 0; x < PRG32_GAME_W; ++x) {
            uint16_t c = g_palette[g_c6_fb[y][x]];
            unsigned r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
            fputc((int)((r << 3) | (r >> 2)), f);
            fputc((int)((g << 2) | (g >> 4)), f);
            fputc((int)((b << 3) | (b >> 2)), f);
        }
    }
    return fclose(f);
}
