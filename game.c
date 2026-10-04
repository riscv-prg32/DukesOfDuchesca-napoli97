/*
 * DukesOfDuchesca-napoli97 -- a PRG32 cartridge.
 *
 * Top-down car-chase homage to "The Dukes of Hazzard", relocated to Napoli
 * in 1997: drive a pimped-up white Fiat 500 across a stylised Napoli,
 * collecting party paraphernalia while dodging scooter gangs and curious
 * police, and keep an eye on the fuel gauge.
 *
 * Graphics are palette-indexed. On the ESP32-C6 the framebuffer stores one
 * byte per pixel and the firmware maps every RGB565 colour to a cell of its
 * 6x6x6 cube, so the city is drawn with prg32_gfx_rect_indexed() through
 * palette entries the cube never uses (IX_*), and every sprite colour is
 * written into the entry of its own cell (program()). The board then shows
 * the authored colours, exactly as QEMU does, and the whole picture can be
 * recoloured by rewriting a few palette entries: sunset turning into night,
 * lit windows, the shimmering gulf, the disco floor, sirens, flashes, fades.
 *
 * Portable cartridges are loaded at different addresses: there are no
 * pointer tables in static data, strings come from functions and sprite
 * descriptors are filled at run time.
 *
 * The cartridge builder accepts a single C translation unit, so the pure
 * map module is pulled in by #include.
 */
#include "prg32.h"
#include "citymap.c"
#include "assets.h"

#define GAME_ID "dukes-napoli97"
#define TICK_MS 33u
#define MAX_STEPS_PER_UPDATE 4

/* ---- small helpers (freestanding: no libc) ------------------------------ */
#define Q4 16

static int duke_abs(int v) { return v < 0 ? -v : v; }
static int duke_min(int a, int b) { return a < b ? a : b; }
static int duke_max(int a, int b) { return a > b ? a : b; }
static int duke_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int duke_floor_div8(int v) { return v >= 0 ? v >> 3 : -(((-v) + 7) >> 3); }
static int duke_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static char *duke_utoa(unsigned v, char *buf, int digits) {
    for (int i = digits - 1; i >= 0; --i) { buf[i] = (char)('0' + v % 10u); v /= 10u; }
    buf[digits] = 0;
    return buf;
}

#define WORLD_PX_W (CM_WORLD_COLS * CM_TILE_PX)
#define WORLD_PX_H (CM_WORLD_ROWS * CM_TILE_PX)

/* ---- palette ------------------------------------------------------------
 * Entries 0..7 are the firmware's named colours and 16..231 its 6x6x6 cube.
 * 8..15 and 232..255 are never chosen by the firmware's RGB565 mapping, so
 * the cartridge owns them. */
enum {
    IX_ASPHALT = 8, IX_LINE, IX_KERB, IX_ROOF0, IX_ROOF1, IX_ROOF2, IX_ROOF3, IX_SHADOW,
    IX_LIGHT = 232, IX_WINDOW, IX_WINDOW2, IX_PIAZZA, IX_PAVE, IX_PARK, IX_TREE, IX_GAS,
    IX_PARTY0, IX_PARTY1, IX_PROM, IX_RAIL, IX_SEA0, IX_SEA1, IX_SEA2, IX_SMOKE,
    IX_SPARK, IX_RADAR, IX_SKY0, /* ..IX_SKY5 = 255: the title's sunset bands */
};
#define IX_BLACK 0
#define IX_WHITE 1
#define IX_RED 2
#define IX_GREEN 3
#define IX_YELLOW 5
#define IX_MAGENTA 7

#define C_NIGHT 0x0846
#define C_GOLD 0xFC40
#define C_WINDOW_LIT 0xFEA8
#define DUSK_MAX 20
#define DUSK_TICKS 200 /* one step of dusk every ~6.6 s: night falls in ~2 min */

static uint8_t s_fade;          /* 0 = black .. 16 = full picture */
static uint8_t s_flash;         /* 8 = white .. 0 = none */
static uint8_t s_dusk;          /* 0 = golden hour .. DUSK_MAX = night */
static uint16_t w_car[DUKE_CAR_COLOURS], w_scooter[DUKE_SCOOTER_COLOURS];
static uint16_t w_police[DUKE_POLICE_COLOURS], w_villa[DUKE_VILLA_COLOURS];
static uint16_t w_items[CM_ITEM_COUNT], w_gas;

static uint16_t mix(uint16_t a, uint16_t b, int t, int n) {
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)(((ar + (br - ar) * t / n) << 11) |
                      ((ag + (bg - ag) * t / n) << 5) | (ab + (bb - ab) * t / n));
}

/* Flash and fade, applied to every colour after its own lighting. */
static uint16_t fx(uint16_t c) {
    if (s_flash) c = mix(c, PRG32_COLOR_WHITE, s_flash, 8);
    if (s_fade < 16) c = mix(PRG32_COLOR_BLACK, c, s_fade, 16);
    return c;
}

/* Daylight for things that do not shine: warm at sunset, navy at night.
 * `depth` is how dark full night makes it, in 32nds. */
static uint16_t lit(uint16_t c, int depth) {
    if (s_dusk < 6) c = mix(c, C_GOLD, 6 - s_dusk, 30);
    return fx(mix(c, C_NIGHT, s_dusk * depth / DUSK_MAX, 32));
}

/* The palette entry the firmware chooses for an RGB565 colour: a named
 * colour or a cube cell (prg32_gfx_index_for_rgb565_unlocked). */
static uint8_t index_of(uint16_t c) {
    unsigned r = (unsigned)(c >> 11) * 5u / 31u;
    unsigned g = (unsigned)((c >> 5) & 63) * 5u / 63u;
    unsigned b = (unsigned)(c & 31) * 5u / 31u;
    if (c == PRG32_COLOR_BLACK) return 0;
    if (c == PRG32_COLOR_WHITE) return 1;
    if (c == PRG32_COLOR_RED) return 2;
    if (c == PRG32_COLOR_GREEN) return 3;
    if (c == PRG32_COLOR_BLUE) return 4;
    if (c == PRG32_COLOR_YELLOW) return 5;
    if (c == PRG32_COLOR_CYAN) return 6;
    if (c == PRG32_COLOR_MAGENTA) return 7;
    return (uint8_t)(16u + r * 36u + g * 6u + b);
}

/* Put a colour in the entry the firmware will choose for it, so a sprite
 * drawn with it shows that exact colour on the indexed framebuffer too.
 * Two colours can fall in the same cell (mostly while fading): the first one
 * programmed in a frame owns the cell and the second is drawn with it, which
 * keeps the indexed framebuffer and QEMU's RGB surface identical. */
static uint16_t s_cell[256];
static uint8_t s_cell_used[256];

static uint16_t program(uint16_t c) {
    uint8_t index = index_of(c);
    if (index < 16) return c;
    if (s_cell_used[index]) return s_cell[index];
    s_cell_used[index] = 1;
    s_cell[index] = c;
    prg32_palette_set(index, c);
    return c;
}

static void group(uint16_t *work, const uint16_t *authored, int n, int depth) {
    work[0] = 0;
    for (int i = 1; i < n; ++i) work[i] = program(lit(authored[i], depth));
}

/* The whole palette is rebuilt every frame: about seventy entries. */
static void palette_apply(void) {
    static const uint16_t roofs[4] = {0xC2A6, 0xE5CD, 0xDC71, 0x9CF2};
    for (int i = 16; i < 232; ++i) s_cell_used[i] = 0;
    prg32_palette_set(IX_ASPHALT, lit(0x39E8, 18));
    prg32_palette_set(IX_LINE, lit(0xEF2A, 12));
    prg32_palette_set(IX_KERB, lit(0x9CD3, 16));
    for (int i = 0; i < 4; ++i) prg32_palette_set((uint8_t)(IX_ROOF0 + i), lit(roofs[i], 20));
    prg32_palette_set(IX_SHADOW, lit(0x4A28, 22));
    prg32_palette_set(IX_LIGHT, lit(0xFF7A, 18));
    prg32_palette_set(IX_PIAZZA, lit(0xD69A, 18));
    prg32_palette_set(IX_PAVE, lit(0xB596, 18));
    prg32_palette_set(IX_PARK, lit(0x3D86, 20));
    prg32_palette_set(IX_TREE, lit(0x1B43, 20));
    prg32_palette_set(IX_GAS, lit(0xFD00, 8));
    prg32_palette_set(IX_PROM, lit(0xE6F8, 18));
    prg32_palette_set(IX_RAIL, lit(0xFFDF, 14));
    prg32_palette_set(IX_SMOKE, lit(0xBDF7, 12));
    prg32_palette_set(IX_RADAR, fx(0x2945));

    group(w_car, duke_car_palette, DUKE_CAR_COLOURS, 8);
    group(w_scooter, duke_scooter_palette, DUKE_SCOOTER_COLOURS, 10);
    group(w_police, duke_police_palette, DUKE_POLICE_COLOURS, 10);
    group(w_villa, duke_villa_palette, DUKE_VILLA_COLOURS, 12);
    /* Lamps and lit windows shine: no daylight on them. */
    w_car[4] = program(fx(duke_car_palette[4]));
    w_scooter[3] = program(fx(duke_scooter_palette[3]));
    w_police[6] = program(fx(duke_police_palette[6]));
    w_villa[3] = program(fx(duke_villa_palette[3]));
    for (int i = 0; i < CM_ITEM_COUNT; ++i) w_items[i] = program(fx(duke_item_colours[i]));
    w_gas = program(fx(PRG32_COLOR_WHITE));
}

/* Entries that move every frame, whatever the state of the game. */
static void palette_animate(uint32_t t) {
    static const uint16_t disco[6] = {0xF81F, 0xFFE0, 0x07FF, 0xF800, 0x07E0, 0x781F};
    static const uint16_t sea[3] = {0x12B5, 0x2BD9, 0x7DDD};
    static const uint16_t lamps[3] = {0xF9E7, 0x47E9, 0x4C5F};
    int phase = (int)(t >> 3);
    /* The gulf: a deep base and two glints that swap, so the water shimmers. */
    prg32_palette_set(IX_SEA0, lit(sea[0], 16));
    prg32_palette_set(IX_SEA1, lit(sea[1 + (phase & 1)], 10));
    prg32_palette_set(IX_SEA2, lit(sea[2 - (phase & 1)], 10));
    /* The villa's dance floor and string lights. */
    prg32_palette_set(IX_PARTY0, fx(disco[phase % 6]));
    prg32_palette_set(IX_PARTY1, fx(disco[(phase + 3) % 6]));
    for (int i = 0; i < 3; ++i)
        w_villa[DUKE_VILLA_LIGHT + i] = program(fx(lamps[(i + phase) % 3]));
    /* Windows come on one group at a time as night falls; one group flickers. */
    prg32_palette_set(IX_WINDOW, fx(mix(0x3A2B, C_WINDOW_LIT, s_dusk, DUSK_MAX)));
    prg32_palette_set(IX_WINDOW2, fx(s_dusk < 8 ? 0x3A2B
                                     : ((t & 63u) < 4 ? 0x9BC6 : C_WINDOW_LIT)));
    prg32_palette_set(IX_SPARK, fx((t & 2u) ? PRG32_COLOR_WHITE : PRG32_COLOR_YELLOW));
    /* The police light bar: the two lamps trade places. */
    w_police[3] = program(fx(duke_police_palette[(t & 8u) ? 3 : 4]));
    w_police[4] = program(fx(duke_police_palette[(t & 8u) ? 4 : 3]));
}

/* ---- audio --------------------------------------------------------------
 * Tracker voices 0..4 carry the music (instrument n on channel n); voices
 * 5..7 are the game's own: the engine, tonal effects and noise. Effects are
 * panned by where they happen on screen. */
enum { TR_TITLE = 0, TR_DRIVE, TR_PARTY, TR_BUSTED };
enum { CH_ENGINE = 5, CH_FX = 6, CH_NOISE = 7 };
enum { I_ENGINE = 5, I_BLIP = 6, I_NOISE = 7, I_HORN = 8, I_SIREN = 9, I_CHIME = 10 };

static int s_music = -1, s_tempo;
static uint8_t s_sfx_left[3];
static int s_engine_note;

static void music(int track) {
    if (s_music == track) return;
    for (uint8_t ch = 0; ch < CH_ENGINE; ++ch) prg32_audio_note_off(ch);
    s_music = track;
    s_tempo = 0;
    prg32_audio_play_track((uint16_t)track);
}

static int pan_at(int screen_x) { return duke_clamp((screen_x - 160) * 2 / 5, -64, 63); }

/* Start an effect; ticks == 0 holds the note until sfx_stop(). */
static void sfx(int ch, int instrument, int note, int volume, int screen_x, int ticks) {
    prg32_audio_note_on_pan((uint8_t)ch, (uint8_t)instrument, (uint8_t)note,
                            (uint8_t)volume, (int8_t)pan_at(screen_x));
    s_sfx_left[ch - CH_ENGINE] = (uint8_t)ticks;
}

static void sfx_stop(int ch) {
    prg32_audio_note_off((uint8_t)ch);
    s_sfx_left[ch - CH_ENGINE] = 0;
}

static void sfx_tick(void) {
    for (int i = 0; i < 3; ++i)
        if (s_sfx_left[i] && --s_sfx_left[i] == 0) prg32_audio_note_off((uint8_t)(CH_ENGINE + i));
}

static void engine_off(void) {
    if (s_engine_note) sfx_stop(CH_ENGINE);
    s_engine_note = 0;
}

/* ---- game state ---------------------------------------------------------
 */
enum { ST_TITLE = 0, ST_PLAYING, ST_PAUSED, ST_WIN, ST_LOSE };
enum { REASON_NONE = 0, REASON_SCOOTER, REASON_POLICE, REASON_GAS };
enum { KIND_SCOOTER = 0, KIND_POLICE };
enum { MSG_NONE = 0, MSG_ITEM, MSG_GAS, MSG_ALL, MSG_LOW, MSG_HIT, MSG_NEED };

#define MAX_SPEED_Q4 56
#define MAX_REVERSE_Q4 24
#define ACCEL_Q4 3
#define BRAKE_Q4 6
#define FRICTION_Q4 1

#define FUEL_MAX 3600
#define FUEL_REFILL_RATE 40
#define TROUBLE_MAX 3
#define INVULN_TICKS 60
#define STALL_TICKS 90
#define HONK_COOLDOWN 45
#define HONK_RADIUS 80
#define SCARED_TICKS 100
#define PATIENCE_TICKS 600 /* a chaser that has not caught the car in 20 s gives up */

#define MAX_ENEMIES 4
#define MAX_PARTICLES 32

typedef struct {
    int32_t x_q4, y_q4;
    int16_t speed_q4;
    uint8_t heading; /* 0..31, 0 = north, clockwise */
} duke_car_t;

typedef struct {
    int16_t x, y;      /* world pixels; always on a tile-centre line */
    uint8_t kind, active, dir, scared, acc;
    uint16_t patience; /* ticks before the chaser gives up */
} duke_enemy_t;

typedef struct {
    int16_t x_q2, y_q2;
    int8_t vx, vy;
    uint8_t life, ix;
} duke_particle_t;

static uint8_t s_state;
static uint8_t s_inited;
static uint32_t s_last_input, s_latched, s_last_ms, s_acc_ms;
static uint32_t s_tick;       /* every simulation step, in every state */
static uint32_t s_play_ticks; /* steps spent driving in this run */

static duke_car_t s_player;
static int s_camera_x, s_camera_y;
static int s_fuel, s_trouble, s_invuln, s_stall, s_shake, s_honk_cd, s_gas_tick;
static uint8_t s_collected[CM_ITEM_COUNT];
static int s_collected_count, s_lose_reason, s_score, s_best;
static int s_msg, s_msg_arg, s_msg_ticks;
static uint8_t s_enemies_on = 1;
static duke_enemy_t s_enemies[MAX_ENEMIES];
static duke_particle_t s_particles[MAX_PARTICLES];
static int s_spawn_timer, s_danger;
static uint32_t s_rng = 0x1997u;

static uint32_t rnd(void) {
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng >> 8;
}

static int player_x(void) { return (int)(s_player.x_q4 / Q4); }
static int player_y(void) { return (int)(s_player.y_q4 / Q4); }

static void say(int msg, int arg, int ticks) {
    s_msg = msg; s_msg_arg = arg; s_msg_ticks = ticks;
}

static const char *item_name(int i) {
    switch (i) {
    case 0: return "BEER";
    case 1: return "WINE";
    case 2: return "SANGRIA PAPELIS";
    case 3: return "AMPLIFIER";
    case 4: return "LOUDSPEAKERS";
    case 5: return "MIXER";
    case 6: return "DISCO LIGHTS";
    default: return "NICE GIRLS";
    }
}

static void particle(int x, int y, int vx, int vy, int life, int ix) {
    for (int i = 0; i < MAX_PARTICLES; ++i) {
        duke_particle_t *p = &s_particles[i];
        if (p->life) continue;
        p->x_q2 = (int16_t)(x * 4); p->y_q2 = (int16_t)(y * 4);
        p->vx = (int8_t)vx; p->vy = (int8_t)vy;
        p->life = (uint8_t)life; p->ix = (uint8_t)ix;
        return;
    }
}

static void burst(int x, int y, int count, int ix) {
    for (int i = 0; i < count; ++i) {
        uint32_t r = rnd();
        particle(x, y, (int)(r & 15u) - 8, (int)((r >> 4) & 15u) - 8, 10 + (int)((r >> 8) & 7u), ix);
    }
}

static void update_camera(void) {
    s_camera_x = duke_clamp(player_x() - PRG32_GAME_W / 2, 0, WORLD_PX_W - PRG32_GAME_W);
    s_camera_y = duke_clamp(player_y() - PRG32_GAME_H / 2, 0, WORLD_PX_H - PRG32_GAME_H);
}

static void reset_game(void) {
    s_player.x_q4 = (int32_t)(cm_start_point.x * CM_TILE_PX + 4) * Q4;
    s_player.y_q4 = (int32_t)(cm_start_point.y * CM_TILE_PX + 4) * Q4;
    s_player.speed_q4 = 0;
    s_player.heading = 0;
    s_fuel = FUEL_MAX;
    s_trouble = s_invuln = s_stall = s_shake = s_honk_cd = s_gas_tick = 0;
    s_lose_reason = REASON_NONE;
    s_collected_count = 0;
    s_score = 0;
    s_play_ticks = 0;
    s_danger = 0;
    s_dusk = 0;
    for (int i = 0; i < CM_ITEM_COUNT; ++i) s_collected[i] = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i) s_enemies[i].active = 0;
    for (int i = 0; i < MAX_PARTICLES; ++i) s_particles[i].life = 0;
    s_spawn_timer = 120;
    say(MSG_NONE, 0, 0);
    update_camera();
}

static void enter(int state) {
    s_state = (uint8_t)state;
    s_fade = 0;
    s_flash = 0;
    engine_off();
    sfx_stop(CH_FX);
    sfx_stop(CH_NOISE);
    if (state == ST_TITLE) { s_dusk = 0; music(TR_TITLE); }
    if (state == ST_PLAYING) music(TR_DRIVE);
    if (state == ST_WIN) music(TR_PARTY);
    if (state == ST_LOSE) music(TR_BUSTED);
}

static void finish(int state) {
    if (s_score > s_best) s_best = s_score;
    if (s_score > 0) prg32_score_submit_current_player(GAME_ID, (uint32_t)s_score);
    enter(state);
    s_fade = 16; /* the result appears over the scene, which stays lit */
}

/* ---- the player ---------------------------------------------------------
 */
static void update_player(uint32_t input) {
    duke_car_t *c = &s_player;
    if (s_fuel > 0 && (input & PRG32_BTN_UP)) {
        c->speed_q4 = (int16_t)duke_min(c->speed_q4 + ACCEL_Q4, MAX_SPEED_Q4);
    } else if (s_fuel > 0 && (input & PRG32_BTN_DOWN)) {
        c->speed_q4 = (int16_t)duke_max(c->speed_q4 - BRAKE_Q4, -MAX_REVERSE_Q4);
    } else {
        /* Out of fuel the car coasts to a stop twice as fast. */
        int drag = s_fuel > 0 ? FRICTION_Q4 : 2 * FRICTION_Q4;
        if (c->speed_q4 > 0) c->speed_q4 = (int16_t)duke_max(0, c->speed_q4 - drag);
        else if (c->speed_q4 < 0) c->speed_q4 = (int16_t)duke_min(0, c->speed_q4 + drag);
    }

    /* The wheel always answers, so the car can be turned out of a corner;
     * standing still it turns at half rate. */
    if (duke_abs(c->speed_q4) >= 4 || (s_tick & 1u)) {
        if (input & PRG32_BTN_LEFT) c->heading = (uint8_t)((c->heading + 31) & 31);
        if (input & PRG32_BTN_RIGHT) c->heading = (uint8_t)((c->heading + 1) & 31);
    }

    int dx = (cm_cos_table[c->heading] * c->speed_q4) / 256;
    int dy = (cm_sin_table[c->heading] * c->speed_q4) / 256;
    int blocked = 0;

    /* Axis-separated slide: motion accumulates in Q4, solidity is tested on
     * whole pixels straight from the procedural map. */
    if (!cm_box_blocked((int)((c->x_q4 + dx) / Q4), player_y())) c->x_q4 += dx;
    else blocked = 1;
    if (!cm_box_blocked(player_x(), (int)((c->y_q4 + dy) / Q4))) c->y_q4 += dy;
    else blocked = 1;
    if (blocked && duke_abs(c->speed_q4) > 8) {
        if (duke_abs(c->speed_q4) > 28) {
            burst(player_x(), player_y(), 4, IX_SPARK);
            sfx(CH_NOISE, I_NOISE, 40, 120, 160, 3);
        }
        c->speed_q4 = (int16_t)(c->speed_q4 * 3 / 4);
    }

    /* Exhaust while accelerating. */
    if ((input & PRG32_BTN_UP) && s_fuel > 0 && (s_tick & 3u) == 0) {
        particle(player_x() - cm_cos_table[c->heading] * 7 / 256,
                 player_y() - cm_sin_table[c->heading] * 7 / 256,
                 (int)(rnd() & 3u) - 1, (int)(rnd() & 3u) - 1, 12, IX_SMOKE);
    }

    /* Fuel: an idle drain plus a speed-proportional term. */
    if (s_fuel > 0) {
        s_fuel = duke_max(0, s_fuel - 1 - (duke_abs(c->speed_q4) >> 5));
        if (s_fuel == FUEL_MAX / 5) say(MSG_LOW, 0, 90);
    }
    if (cm_tile_at(player_x() >> 3, player_y() >> 3) == CM_T_GAS && s_fuel < FUEL_MAX) {
        s_fuel = duke_min(FUEL_MAX, s_fuel + FUEL_REFILL_RATE);
        if (s_gas_tick == 0) {
            sfx(CH_FX, I_BLIP, 60 + s_fuel * 24 / FUEL_MAX, 150, 160, 2);
            s_gas_tick = 5;
            say(MSG_GAS, 0, 20);
        }
    }
    if (s_gas_tick > 0) s_gas_tick--;
    if (s_fuel < FUEL_MAX / 5 && s_fuel > 0 && (s_tick & 31u) == 0)
        sfx(CH_FX, I_BLIP, 84, 110, 160, 2);

    if (s_fuel <= 0 && c->speed_q4 == 0) {
        if (++s_stall > STALL_TICKS) {
            s_lose_reason = REASON_GAS;
            finish(ST_LOSE);
            return;
        }
    } else {
        s_stall = 0;
    }

    /* The engine: one held voice, re-pitched when the speed crosses a step. */
    int note = 31 + duke_abs(c->speed_q4) / 5;
    if (note != s_engine_note) {
        s_engine_note = note;
        sfx(CH_ENGINE, I_ENGINE, note, 96, 160, 0);
    }
}

/* ---- scooters and police ------------------------------------------------
 * They drive the street grid tile by tile, like the ghosts of a maze game:
 * at each tile centre they take the open direction that brings them closest
 * to their target, never reversing unless the street is a dead end. */
static const int8_t dir_dx[4] = {0, 1, 0, -1};
static const int8_t dir_dy[4] = {-1, 0, 1, 0};

static void enemy_choose(duke_enemy_t *e) {
    int tx = e->x >> 3, ty = e->y >> 3;
    int goal_x = player_x(), goal_y = player_y();
    if (e->kind == KIND_POLICE) {
        /* The police cut the car off: they aim ahead of it. */
        goal_x += cm_cos_table[s_player.heading] * s_player.speed_q4 / 256;
        goal_y += cm_sin_table[s_player.heading] * s_player.speed_q4 / 256;
    }
    int best = -1, fallback = -1, random_pick = -1, open = 0;
    int32_t best_score = 0;
    int stray = (rnd() & 7u) == 0;
    for (int d = 0; d < 4; ++d) {
        int nx = tx + dir_dx[d], ny = ty + dir_dy[d];
        if (cm_tile_is_solid(cm_tile_at(nx, ny))) continue;
        if (d == ((e->dir + 2) & 3)) { fallback = d; continue; }
        open++;
        if ((rnd() % (uint32_t)open) == 0) random_pick = d;
        int32_t ddx = nx * 8 + 4 - goal_x, ddy = ny * 8 + 4 - goal_y;
        int32_t score = ddx * ddx + ddy * ddy;
        if (e->scared) score = -score;
        if (best < 0 || score < best_score) { best = d; best_score = score; }
    }
    if (best >= 0 && stray) best = random_pick;
    if (best < 0) best = fallback;
    if (best >= 0) e->dir = (uint8_t)best;
}

static void spawn_enemy(void) {
    int slot = -1, active = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        if (s_enemies[i].active) active++;
        else if (slot < 0) slot = i;
    }
    if (slot < 0 || active >= 2 + s_collected_count / 3) return;
    for (int attempt = 0; attempt < 6; ++attempt) {
        int dx = (int)(rnd() % 61u) - 30, dy = (int)(rnd() % 61u) - 30;
        if (duke_abs(dx) < 22 && duke_abs(dy) < 15) continue; /* off screen only */
        int tx = (player_x() >> 3) + dx, ty = (player_y() >> 3) + dy;
        if (cm_tile_is_solid(cm_tile_at(tx, ty))) continue;
        duke_enemy_t *e = &s_enemies[slot];
        e->x = (int16_t)(tx * 8 + 4);
        e->y = (int16_t)(ty * 8 + 4);
        e->active = 1;
        e->scared = 0;
        e->acc = 0;
        e->patience = PATIENCE_TICKS;
        e->dir = (uint8_t)(rnd() & 3u);
        /* Twice as many scooters as police cars. */
        e->kind = (rnd() % 3u) == 0 ? KIND_POLICE : KIND_SCOOTER;
        return;
    }
}

static void update_enemies(uint32_t pressed) {
    int px = player_x(), py = player_y();
    int honk = (pressed & PRG32_BTN_A) && s_honk_cd == 0;

    if (s_enemies_on && --s_spawn_timer <= 0) {
        spawn_enemy();
        s_spawn_timer = 90 + (int)(rnd() % 120u) - s_collected_count * 6;
    }
    if (s_honk_cd > 0) s_honk_cd--;
    if (honk) {
        s_honk_cd = HONK_COOLDOWN;
        sfx(CH_FX, I_HORN, 65, 200, 160, 7);
    }

    int nearest = 9999, siren_x = -1;
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        duke_enemy_t *e = &s_enemies[i];
        if (!e->active) continue;
        int dist = duke_max(duke_abs(px - e->x), duke_abs(py - e->y));
        if (dist > 300 || --e->patience == 0) { e->active = 0; continue; }
        if (honk && e->kind == KIND_SCOOTER && dist < HONK_RADIUS && !e->scared) {
            e->scared = SCARED_TICKS;
            e->dir = (uint8_t)((e->dir + 2) & 3);
            s_score += 50;
        }
        if (e->scared) e->scared--;

        /* Scooters are quick, the police relentless; both get keener as the
         * boot fills up. Always slower than the Fiat flat out. */
        int speed = (e->kind == KIND_SCOOTER ? 34 : 29) + s_collected_count;
        if (e->scared) speed = 44;
        int budget = e->acc + speed;
        while (budget >= Q4) {
            budget -= Q4;
            if ((e->x & 7) == 4 && (e->y & 7) == 4) enemy_choose(e);
            int nx = e->x + dir_dx[e->dir], ny = e->y + dir_dy[e->dir];
            if (cm_tile_is_solid(cm_tile_at(nx >> 3, ny >> 3))) break;
            e->x = (int16_t)nx;
            e->y = (int16_t)ny;
        }
        e->acc = (uint8_t)(budget & (Q4 - 1));

        dist = duke_max(duke_abs(px - e->x), duke_abs(py - e->y));
        if (!e->scared && dist < nearest) nearest = dist;
        if (e->kind == KIND_POLICE && dist < 150) siren_x = e->x - s_camera_x;

        if (s_invuln == 0 && dist < 2 * CM_CAR_HALF) {
            s_trouble++;
            s_invuln = INVULN_TICKS;
            s_shake = 12;
            s_flash = 6;
            s_player.speed_q4 = (int16_t)(s_player.speed_q4 / 3);
            e->active = 0;
            burst(e->x, e->y, 10, IX_SPARK);
            sfx(CH_NOISE, I_NOISE, 30, 230, e->x - s_camera_x, 8);
            say(MSG_HIT, e->kind, 60);
            if (s_trouble >= TROUBLE_MAX) {
                s_lose_reason = e->kind == KIND_SCOOTER ? REASON_SCOOTER : REASON_POLICE;
                finish(ST_LOSE);
                return;
            }
        }
    }

    /* A two-tone siren from wherever the nearest police car is. */
    if (siren_x >= 0 && (s_tick & 7u) == 0 && s_sfx_left[CH_NOISE - CH_ENGINE] == 0)
        sfx(CH_NOISE, I_SIREN, (s_tick & 8u) ? 76 : 71, 120, siren_x, 6);

    /* The band plays faster when someone is on the Fiat's tail. */
    s_danger = nearest < 110;
    int tempo = s_danger ? 172 : 150;
    if (tempo != s_tempo) {
        s_tempo = tempo;
        prg32_audio_set_tempo((uint16_t)tempo);
    }
}

static void update_items_and_party(void) {
    int px = player_x(), py = player_y();
    for (int i = 0; i < CM_ITEM_COUNT; ++i) {
        if (s_collected[i]) continue;
        int ix = cm_item_points[i].x * CM_TILE_PX + 4;
        int iy = cm_item_points[i].y * CM_TILE_PX + 4;
        if (duke_abs(px - ix) < 9 && duke_abs(py - iy) < 9) {
            s_collected[i] = 1;
            s_collected_count++;
            s_score += 250;
            burst(ix, iy, 12, IX_SPARK);
            sfx(CH_FX, I_CHIME, 72 + s_collected_count * 2, 210, ix - s_camera_x, 6);
            say(s_collected_count == CM_ITEM_COUNT ? MSG_ALL : MSG_ITEM, i, 75);
        }
    }

    if (cm_tile_at(px >> 3, py >> 3) == CM_T_PARTY) {
        if (s_collected_count >= CM_ITEM_COUNT) {
            int seconds = (int)(s_play_ticks * TICK_MS / 1000u);
            s_score += 2000 + duke_max(0, 3000 - seconds * 10) + s_fuel / 4 -
                       s_trouble * 300;
            finish(ST_WIN);
        } else if (s_msg_ticks == 0) {
            say(MSG_NEED, CM_ITEM_COUNT - s_collected_count, 45);
        }
    }
}

static void update_particles(void) {
    for (int i = 0; i < MAX_PARTICLES; ++i) {
        duke_particle_t *p = &s_particles[i];
        if (!p->life) continue;
        p->life--;
        p->x_q2 = (int16_t)(p->x_q2 + p->vx);
        p->y_q2 = (int16_t)(p->y_q2 + p->vy);
    }
}

/* One fixed 33 ms simulation step. */
static void step(uint32_t input, uint32_t pressed) {
    s_tick++;
    sfx_tick();
    if (s_fade < 16) s_fade = (uint8_t)duke_min(16, s_fade + 2);
    if (s_flash) s_flash--;
    if (s_shake) s_shake--;
    if (s_msg_ticks) s_msg_ticks--;

    switch (s_state) {
    case ST_TITLE:
        if (pressed & (PRG32_BTN_START | PRG32_BTN_A)) {
            reset_game();
            enter(ST_PLAYING);
        }
        break;
    case ST_PLAYING:
        if (pressed & PRG32_BTN_START) {
            engine_off();
            s_state = ST_PAUSED;
            break;
        }
        s_play_ticks++;
        if (s_dusk < DUSK_MAX && s_play_ticks % DUSK_TICKS == 0) s_dusk++;
        if (s_invuln > 0) s_invuln--;
        update_player(input);
        if (s_state == ST_PLAYING) update_enemies(pressed);
        if (s_state == ST_PLAYING) update_items_and_party();
        update_particles();
        update_camera();
        break;
    case ST_PAUSED:
        if (pressed & PRG32_BTN_START) s_state = ST_PLAYING;
        break;
    case ST_WIN:
        if ((s_tick & 3u) == 0) {
            /* Confetti over the villa. */
            uint32_t r = rnd();
            particle(s_camera_x + (int)(r % 320u), s_camera_y, (int)((r >> 9) & 3u) - 1, 6,
                     40, (r & 1u) ? IX_PARTY0 : IX_PARTY1);
        }
        update_particles();
        /* fall through */
    case ST_LOSE:
        if (pressed & (PRG32_BTN_START | PRG32_BTN_A)) enter(ST_TITLE);
        break;
    }
}

/* ---- drawing ------------------------------------------------------------
 */
static void box(int x, int y, int w, int h, int index) {
    prg32_gfx_rect_indexed(x, y, w, h, (uint8_t)index);
}

static void sprite4(const uint8_t *pixels, const uint16_t *pal, int colours, int w, int h,
                    int frames, int frame, int x, int y) {
    prg32_indexed_sprite_t s;
    s.pixels = pixels; s.palette = pal;
    s.width = (uint16_t)w; s.height = (uint16_t)h;
    s.frame_count = (uint16_t)frames; s.palette_count = (uint16_t)colours;
    s.bits_per_pixel = PRG32_SPRITE_BPP_4;
    s.transparent_index = 0;
    prg32_sprite_draw_indexed(x, y, &s, (uint32_t)frame);
}

/* An 8x8 one-bit icon with a transparent background. */
static void icon(const uint8_t *bits, uint16_t colour, int x, int y) {
    uint16_t pal[2];
    prg32_indexed_sprite_t s;
    pal[0] = 0; pal[1] = colour;
    s.pixels = bits; s.palette = pal;
    s.width = 8; s.height = 8; s.frame_count = 1; s.palette_count = 2;
    s.bits_per_pixel = PRG32_SPRITE_BPP_1;
    s.transparent_index = 0;
    prg32_sprite_draw_indexed(x, y, &s, 0);
}

static void text(int x, int y, const char *s, uint16_t fg) {
    prg32_gfx_text8(x, y, s, fg, PRG32_COLOR_BLACK);
}

static void text_centred(int y, const char *s, uint16_t fg) {
    text((PRG32_GAME_W - duke_strlen(s) * 8) / 2, y, s, fg);
}

/* The city, drawn from the procedural map every frame: one indexed rectangle
 * per run of equal ground along a row, then the small details. */
#define VIEW_COLS (PRG32_GAME_W / CM_TILE_PX + 1)
#define VIEW_ROWS (PRG32_GAME_H / CM_TILE_PX + 1)
static uint8_t s_grid[VIEW_ROWS + 2][VIEW_COLS + 2];

static int ground_index(int kind, int tx, int ty) {
    switch (kind) {
    case CM_T_SEA: return IX_SEA0;
    case CM_T_BUILDING: return IX_ROOF0 + cm_block_style(tx, ty);
    case CM_T_PIAZZA: return IX_PIAZZA;
    case CM_T_PARK: return IX_PARK;
    case CM_T_GAS: return IX_GAS;
    case CM_T_PARTY: return ((tx + ty) & 1) ? IX_PARTY0 : IX_PARTY1;
    case CM_T_PROMENADE: return IX_PROM;
    default: return IX_ASPHALT;
    }
}

static void draw_city(int cam_x, int cam_y) {
    int tx0 = duke_floor_div8(cam_x), ty0 = duke_floor_div8(cam_y);
    int ox = tx0 * 8 - cam_x, oy = ty0 * 8 - cam_y;

    for (int gy = 0; gy < VIEW_ROWS + 2; ++gy)
        for (int gx = 0; gx < VIEW_COLS + 2; ++gx)
            s_grid[gy][gx] = cm_tile_at(tx0 - 1 + gx, ty0 - 1 + gy);

    for (int gy = 1; gy <= VIEW_ROWS; ++gy) {
        int ty = ty0 - 1 + gy, y = oy + (gy - 1) * 8;
        int run_start = 1, run_index = ground_index(s_grid[gy][1], tx0, ty);
        for (int gx = 2; gx <= VIEW_COLS + 1; ++gx) {
            int index = gx <= VIEW_COLS ? ground_index(s_grid[gy][gx], tx0 - 1 + gx, ty) : -1;
            if (index == run_index) continue;
            box(ox + (run_start - 1) * 8, y, (gx - run_start) * 8, 8, run_index);
            run_start = gx;
            run_index = index;
        }
    }

    for (int gy = 1; gy <= VIEW_ROWS; ++gy) {
        int ty = ty0 - 1 + gy, y = oy + (gy - 1) * 8;
        for (int gx = 1; gx <= VIEW_COLS; ++gx) {
            int tx = tx0 - 1 + gx, x = ox + (gx - 1) * 8;
            uint32_t h;
            switch (s_grid[gy][gx]) {
            case CM_T_BUILDING: {
                /* Roofs are lit from the north-west: a bright rim there, the
                 * shaded wall showing along the south and east. */
                int edge = 0;
                if (s_grid[gy - 1][gx] != CM_T_BUILDING) { box(x, y, 8, 1, IX_LIGHT); edge = 1; }
                if (s_grid[gy][gx - 1] != CM_T_BUILDING) { box(x, y, 1, 8, IX_LIGHT); edge = 1; }
                if (s_grid[gy + 1][gx] != CM_T_BUILDING) { box(x, y + 5, 8, 3, IX_SHADOW); edge = 1; }
                if (s_grid[gy][gx + 1] != CM_T_BUILDING) { box(x + 6, y, 2, 8, IX_SHADOW); edge = 1; }
                h = cm_hash(tx, ty);
                if (!edge && (h & 3u) == 0) box(x + 2, y + 2, 3, 3, (h & 4u) ? IX_WINDOW : IX_WINDOW2);
                break;
            }
            case CM_T_PARK:
                if ((tx + ty) & 1) box(x + 1, y + 1, 5, 5, IX_TREE);
                else box(x + 4, y + 4, 3, 3, IX_TREE);
                break;
            case CM_T_PIAZZA:
                if (((tx + ty) & 1) == 0) box(x + 3, y + 3, 2, 2, IX_PAVE);
                break;
            case CM_T_ROAD_LINE:
                box(x + 1, y + 3, 6, 2, IX_LINE);
                break;
            case CM_T_PROMENADE:
                /* A white parapet wherever the lungomare meets the water. */
                if (s_grid[gy + 1][gx] == CM_T_SEA) box(x, y + 6, 8, 2, IX_RAIL);
                if (s_grid[gy - 1][gx] == CM_T_SEA) box(x, y, 8, 2, IX_RAIL);
                if (s_grid[gy][gx + 1] == CM_T_SEA) box(x + 6, y, 2, 8, IX_RAIL);
                if (s_grid[gy][gx - 1] == CM_T_SEA) box(x, y, 2, 8, IX_RAIL);
                break;
            case CM_T_SEA:
                h = cm_hash(tx, ty);
                if ((h & 3u) == 0)
                    box(x + (int)((h >> 4) & 3u), y + (int)((h >> 6) & 7u), 4, 1,
                        (h & 256u) ? IX_SEA1 : IX_SEA2);
                break;
            default:
                break;
            }
        }
    }
}

static void draw_vehicle(const uint8_t *pixels, const uint16_t *pal, int colours,
                         int heading8, int sx, int sy) {
    sprite4(pixels, pal, colours, DUKE_VEHICLE_SIZE, DUKE_VEHICLE_SIZE, DUKE_VEHICLE_FRAMES,
            heading8 & 7, sx - DUKE_VEHICLE_SIZE / 2, sy - DUKE_VEHICLE_SIZE / 2);
}

/* The nearest thing still to fetch: an item, or the villa once the boot is full. */
static int target(int *wx, int *wy) {
    int px = player_x(), py = player_y(), best = -1;
    int32_t best_d = 0;
    for (int i = 0; i < CM_ITEM_COUNT; ++i) {
        if (s_collected[i]) continue;
        int32_t dx = cm_item_points[i].x * 8 + 4 - px, dy = cm_item_points[i].y * 8 + 4 - py;
        int32_t d = dx * dx + dy * dy;
        if (best < 0 || d < best_d) { best = i; best_d = d; }
    }
    if (best < 0) {
        *wx = cm_party_point.x * 8 + 4; *wy = cm_party_point.y * 8 + 4;
        return CM_ITEM_COUNT;
    }
    *wx = cm_item_points[best].x * 8 + 4; *wy = cm_item_points[best].y * 8 + 4;
    return best;
}

static int heading8_for(int dx, int dy) {
    int adx = duke_abs(dx), ady = duke_abs(dy);
    if (adx > ady * 2) return dx > 0 ? 2 : 6;
    if (ady > adx * 2) return dy > 0 ? 4 : 0;
    if (dx > 0) return dy > 0 ? 3 : 1;
    return dy > 0 ? 5 : 7;
}

static void draw_world(void) {
    static const int8_t bob[8] = {0, -1, -2, -2, -1, 0, 1, 1};
    int cam_x = s_camera_x, cam_y = s_camera_y;
    if (s_shake) {
        cam_x += (int)(s_tick & 2u) - 1;
        cam_y += (int)((s_tick >> 1) & 2u) - 1;
    }
    draw_city(cam_x, cam_y);

    sprite4(duke_villa_pixels, w_villa, DUKE_VILLA_COLOURS, DUKE_VILLA_WIDTH, DUKE_VILLA_HEIGHT, 1, 0,
            cm_party_point.x * 8 - DUKE_VILLA_WIDTH / 2 - cam_x,
            cm_party_point.y * 8 - 4 - DUKE_VILLA_HEIGHT - cam_y);

    for (int i = 0; i < CM_GAS_COUNT; ++i)
        icon(duke_gas_icon, w_gas, cm_gas_points[i].x * 8 - cam_x, cm_gas_points[i].y * 8 - cam_y);

    for (int i = 0; i < CM_ITEM_COUNT; ++i) {
        if (s_collected[i]) continue;
        int x = cm_item_points[i].x * 8 - cam_x, y = cm_item_points[i].y * 8 - cam_y;
        /* A pulsing marker under the item, and the item bobbing over it. */
        int r = 6 + (int)((s_tick >> 2) & 3u);
        box(x + 4 - r, y + 4 - r, 2, 2, IX_SPARK);
        box(x + 2 + r, y + 4 - r, 2, 2, IX_SPARK);
        box(x + 4 - r, y + 2 + r, 2, 2, IX_SPARK);
        box(x + 2 + r, y + 2 + r, 2, 2, IX_SPARK);
        box(x - 1, y - 1 + bob[(s_tick >> 2) & 7u], 10, 10, IX_BLACK);
        icon(duke_item_icons[i], w_items[i], x, y + bob[(s_tick >> 2) & 7u]);
    }

    for (int i = 0; i < MAX_PARTICLES; ++i) {
        const duke_particle_t *p = &s_particles[i];
        if (p->life) box(p->x_q2 / 4 - cam_x, p->y_q2 / 4 - cam_y, 2, 2, p->ix);
    }

    for (int i = 0; i < MAX_ENEMIES; ++i) {
        const duke_enemy_t *e = &s_enemies[i];
        if (!e->active) continue;
        if (e->kind == KIND_SCOOTER)
            draw_vehicle(duke_scooter_pixels, w_scooter, DUKE_SCOOTER_COLOURS, e->dir * 2,
                         e->x - cam_x, e->y - cam_y);
        else
            draw_vehicle(duke_police_pixels, w_police, DUKE_POLICE_COLOURS, e->dir * 2,
                         e->x - cam_x, e->y - cam_y);
    }

    int psx = player_x() - cam_x, psy = player_y() - cam_y;
    if (s_invuln == 0 || (s_tick & 2u) == 0)
        draw_vehicle(duke_car_pixels, w_car, DUKE_CAR_COLOURS, (s_player.heading + 2) >> 2, psx, psy);

    /* The compass: an arrow orbiting the car, pointing at what to fetch next. */
    if (s_state == ST_PLAYING) {
        int wx, wy, what = target(&wx, &wy);
        int dx = wx - player_x(), dy = wy - player_y();
        if (duke_abs(dx) > 60 || duke_abs(dy) > 60) {
            int h8 = heading8_for(dx, dy);
            icon(duke_arrow_icons[h8], what == CM_ITEM_COUNT ? PRG32_COLOR_MAGENTA : PRG32_COLOR_YELLOW,
                 psx - 4 + cm_cos_table[h8 * 4] * 22 / 256, psy - 4 + cm_sin_table[h8 * 4] * 22 / 256);
        }
    }
}

static void draw_hud(void) {
    char buf[8];
    box(0, 0, PRG32_GAME_W, 12, IX_BLACK);
    text(2, 2, "GAS", PRG32_COLOR_WHITE);
    int low = s_fuel < FUEL_MAX / 5;
    box(30, 3, 66, 6, IX_RADAR);
    if (!low || (s_tick & 8u))
        box(31, 4, s_fuel * 64 / FUEL_MAX, 4, low ? IX_RED : IX_GREEN);

    for (int i = 0; i < CM_ITEM_COUNT; ++i) {
        if (s_collected[i]) icon(duke_item_icons[i], w_items[i], 104 + i * 10, 2);
        else box(107 + i * 10, 5, 2, 2, IX_RADAR);
    }
    for (int i = 0; i < TROUBLE_MAX; ++i)
        box(194 + i * 9, 3, 7, 6, i < s_trouble ? IX_RED : IX_RADAR);
    text(232, 2, "PTS", PRG32_COLOR_CYAN);
    text(264, 2, duke_utoa((unsigned)s_score, buf, 6), PRG32_COLOR_WHITE);

    /* Radar: the whole city at one pixel per five tiles. */
    int rx = 272, ry = 170;
    box(rx - 1, ry - 1, 46, 28, IX_BLACK);
    box(rx, ry, 44, 26, IX_RADAR);
    for (int i = 0; i < 44; ++i) { /* the gulf, column by column */
        int coast = cm_coast_row(i * 5 + 2) / 5;
        box(rx + i, ry + coast, 1, 26 - coast, IX_SEA0);
    }
    for (int i = 0; i < CM_GAS_COUNT; ++i)
        box(rx + cm_gas_points[i].x / 5, ry + cm_gas_points[i].y / 5, 1, 1, IX_GAS);
    for (int i = 0; i < CM_ITEM_COUNT; ++i)
        if (!s_collected[i] && (s_tick & 8u))
            box(rx + cm_item_points[i].x / 5, ry + cm_item_points[i].y / 5, 2, 2, IX_YELLOW);
    box(rx + cm_party_point.x / 5, ry + cm_party_point.y / 5, 2, 2, IX_MAGENTA);
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (s_enemies[i].active)
            box(rx + s_enemies[i].x / 40, ry + s_enemies[i].y / 40, 1, 1, IX_RED);
    box(rx + player_x() / 40, ry + player_y() / 40, 2, 2, IX_WHITE);

    if (s_msg_ticks && s_state == ST_PLAYING) {
        switch (s_msg) {
        case MSG_ITEM: text_centred(186, item_name(s_msg_arg), w_items[s_msg_arg]); break;
        case MSG_ALL: text_centred(186, "ALL ABOARD - TO THE VILLA!", PRG32_COLOR_MAGENTA); break;
        case MSG_GAS: text_centred(186, "FILL 'ER UP", PRG32_COLOR_GREEN); break;
        case MSG_LOW: text_centred(186, "LOW ON GAS!", PRG32_COLOR_RED); break;
        case MSG_NEED: text_centred(186, "NO PARTY WITHOUT THE GEAR", PRG32_COLOR_YELLOW); break;
        case MSG_HIT:
            text_centred(186, s_msg_arg == KIND_SCOOTER ? "SCOOTER GANG!" : "POLIZIA!", PRG32_COLOR_RED);
            break;
        default: break;
        }
    }
}

static void draw_panel(const char *l1, const char *l2, const char *l3, int accent, uint16_t fg) {
    char buf[8];
    int y = 58;
    box(28, y, PRG32_GAME_W - 56, 74, IX_BLACK);
    box(28, y, PRG32_GAME_W - 56, 2, accent);
    box(28, y + 72, PRG32_GAME_W - 56, 2, accent);
    text_centred(y + 10, l1, fg);
    text_centred(y + 26, l2, PRG32_COLOR_WHITE);
    if (l3) {
        text_centred(y + 42, l3, PRG32_COLOR_WHITE);
    } else {
        text(96, y + 42, "SCORE", PRG32_COLOR_CYAN);
        text(144, y + 42, duke_utoa((unsigned)s_score, buf, 6), PRG32_COLOR_WHITE);
    }
    if (s_tick & 16u) text_centred(y + 58, "PRESS START", PRG32_COLOR_YELLOW);
}

/* The title: a banded sunset over the gulf, the Vesuvio, and a chase. */
static void draw_title(void) {
    static const uint16_t sky[6] = {0x18CE, 0x494F, 0x89CC, 0xD249, 0xFB86, 0xFD64};
    char buf[8];
    for (int i = 0; i < 6; ++i) {
        prg32_palette_set((uint8_t)(IX_SKY0 + i), fx(sky[i]));
        box(0, i * 20, PRG32_GAME_W, 20, IX_SKY0 + i);
    }
    for (int i = 0; i < 20; ++i) /* stars in the darker bands */
        if (((s_tick >> 3) + (uint32_t)i) & 3u)
            box((i * 137) % 320, (i * 53) % 44, 1, 1, IX_WHITE);
    box(236, 84, 20, 20, IX_SPARK); /* the low sun */
    /* The Vesuvio and Monte Somma, as stacked slabs. */
    for (int i = 0; i < 24; ++i) {
        box(150 - i * 3, 72 + i * 2, 34 + i * 6, 2, IX_SHADOW);
        if (i > 5) box(232 - i * 3, 72 + i * 2, 14 + i * 5, 2, IX_SHADOW);
    }
    box(150, 70, 34, 2, IX_KERB);
    box(0, 120, PRG32_GAME_W, 44, IX_SEA0);
    for (int i = 0; i < 40; ++i)
        box((i * 97 + (int)(s_tick >> 2)) % 320, 122 + (i * 31) % 40, 6, 1, (i & 1) ? IX_SEA1 : IX_SEA2);
    box(0, 164, PRG32_GAME_W, 4, IX_RAIL);
    box(0, 168, PRG32_GAME_W, 32, IX_ASPHALT);
    for (int i = 0; i < 8; ++i)
        box(i * 44 - (int)((s_tick * 3u) % 44u), 183, 22, 2, IX_LINE);

    /* The chase along the lungomare. */
    int car_x = (int)((s_tick * 2u) % 520u) - 60;
    draw_vehicle(duke_car_pixels, w_car, DUKE_CAR_COLOURS, 2, car_x, 176);
    draw_vehicle(duke_scooter_pixels, w_scooter, DUKE_SCOOTER_COLOURS, 2, car_x - 44, 190);
    draw_vehicle(duke_police_pixels, w_police, DUKE_POLICE_COLOURS, 2, car_x - 84, 178);

    box(20, 14, 280, 34, IX_BLACK);
    box(20, 14, 280, 2, IX_YELLOW);
    box(20, 46, 280, 2, IX_YELLOW);
    text_centred(21, "D U K E S   O F   D U C H E S C A", PRG32_COLOR_YELLOW);
    text_centred(34, "- NAPOLI, SUMMER 1997 -", PRG32_COLOR_WHITE);
    text_centred(54, "8 PARTY ITEMS. ONE FIAT 500.", PRG32_COLOR_WHITE);
    if (s_best > 0) {
        text(104, 150, "BEST", PRG32_COLOR_CYAN);
        text(144, 150, duke_utoa((unsigned)s_best, buf, 6), PRG32_COLOR_WHITE);
    }
    if (s_tick & 16u) text_centred(106, "PRESS START", PRG32_COLOR_YELLOW);
}

/* ---- exported ABI: dukes_init / dukes_update / dukes_draw -------------- */
void dukes_init(void) {
    static const uint16_t named[8] = {
        PRG32_COLOR_BLACK, PRG32_COLOR_WHITE, PRG32_COLOR_RED, PRG32_COLOR_GREEN,
        PRG32_COLOR_BLUE, PRG32_COLOR_YELLOW, PRG32_COLOR_CYAN, PRG32_COLOR_MAGENTA};
    for (int i = 0; i < 8; ++i) prg32_palette_set((uint8_t)i, named[i]);
    s_inited = 1;
    s_last_input = prg32_input_read(); /* a button still held from the menu is not a press */
    s_latched = 0;
    s_last_ms = prg32_ticks_ms();
    s_acc_ms = 0;
    s_tick = 0;
    s_rng ^= s_last_ms;
    s_music = -1;
    s_engine_note = 0;
    reset_game();
    enter(ST_TITLE);
}

void dukes_update(void) {
    if (!s_inited) dukes_init();
    uint32_t input = prg32_input_read();
    uint32_t now = prg32_ticks_ms();
    /* A press between two simulation steps is kept until a step sees it. */
    s_latched |= input & ~s_last_input;
    s_last_input = input;
    s_acc_ms += now - s_last_ms;
    s_last_ms = now;
    if (s_acc_ms > TICK_MS * MAX_STEPS_PER_UPDATE) s_acc_ms = TICK_MS * MAX_STEPS_PER_UPDATE;
    while (s_acc_ms >= TICK_MS) {
        s_acc_ms -= TICK_MS;
        step(input, s_latched);
        s_latched = 0;
    }
}

void dukes_draw(void) {
    if (!s_inited) return;
    palette_apply();
    palette_animate(s_tick);
    prg32_gfx_clear(PRG32_COLOR_BLACK);

    if (s_state == ST_TITLE) {
        draw_title();
        return;
    }

    draw_world();
    draw_hud();

    if (s_state == ST_PAUSED) {
        draw_panel("PAUSED", "THE NIGHT IS STILL YOUNG", "A: HORN   START: RESUME", IX_WHITE,
                   PRG32_COLOR_CYAN);
    } else if (s_state == ST_WIN) {
        draw_panel("PARTY TIME!", "YOU MADE IT TO THE VILLA", 0, IX_PARTY0, PRG32_COLOR_YELLOW);
    } else if (s_state == ST_LOSE) {
        const char *why = "THEY STOLE THE CINQUECENTO!";
        if (s_lose_reason == REASON_POLICE) why = "BUSTED BY THE POLIZIA!";
        if (s_lose_reason == REASON_GAS) why = "RAN DRY IN THE VICOLI!";
        draw_panel("GAME OVER", why, 0, IX_RED, PRG32_COLOR_RED);
    }
}

#ifdef DUKE_HOST_TEST
/* Accessors compiled in only for tests/host_harness.c. Never defined in the
 * real cartridge build, so this block ships zero bytes to the cartridge. */
int duke_test_state(void) { return s_state; }
int duke_test_fuel(void) { return s_fuel; }
int duke_test_trouble(void) { return s_trouble; }
int duke_test_collected_count(void) { return s_collected_count; }
int duke_test_collected(int i) { return s_collected[i]; }
int duke_test_player_x(void) { return player_x(); }
int duke_test_player_y(void) { return player_y(); }
int duke_test_heading(void) { return s_player.heading; }
int duke_test_score(void) { return s_score; }
int duke_test_dusk(void) { return s_dusk; }
int duke_test_enemy_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i) n += s_enemies[i].active ? 1 : 0;
    return n;
}
int duke_test_enemy(int i, int *x, int *y) {
    *x = s_enemies[i].x; *y = s_enemies[i].y;
    return s_enemies[i].active;
}
void duke_test_enemies(int on) { s_enemies_on = (uint8_t)on; }
void duke_test_set_dusk(int dusk) { s_dusk = (uint8_t)dusk; }
#endif
