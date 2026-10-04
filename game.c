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
static int duke_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static char *duke_utoa(unsigned v, char *buf, int digits) {
    for (int i = digits - 1; i >= 0; --i) { buf[i] = (char)('0' + v % 10u); v /= 10u; }
    buf[digits] = 0;
    return buf;
}

#define WORLD_PX_W (CM_WORLD_COLS * CM_TILE_PX)
#define WORLD_PX_H (CM_WORLD_ROWS * CM_TILE_PX)

/* The world is shown at two screen pixels per world pixel: a tile is 16x16
 * on screen and the viewport covers 160x100 world pixels. Physics stays in
 * world pixels (Q4); only the camera and the drawing know about the zoom. */
#define ZOOM 2
#define TILE_SCREEN (CM_TILE_PX * ZOOM)

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
/* In the city the six sunset entries are the materials of the monuments. */
enum { IX_STONE = IX_SKY0, IX_MARBLE, IX_REDWALL, IX_GLASS, IX_COPPER, IX_TRACK };
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
static uint16_t w_items[CM_ITEM_COUNT], w_gas, w_rauti, w_dim;

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
    prg32_palette_set(IX_STONE, lit(0x9C6E, 18));   /* yellow tuff and piperno */
    prg32_palette_set(IX_MARBLE, lit(0xEF5B, 16));
    prg32_palette_set(IX_REDWALL, lit(0xB226, 18)); /* the red of the Bourbon palaces */
    prg32_palette_set(IX_GLASS, lit(0x5DBB, 10));
    prg32_palette_set(IX_COPPER, lit(0x5C8D, 16));  /* weathered domes */
    prg32_palette_set(IX_TRACK, lit(0x6B2C, 18));

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
    w_rauti = program(fx(0xFAE0));
    w_dim = program(fx(0x4208));
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
enum { REASON_NONE = 0, REASON_SCOOTER, REASON_POLICE, REASON_GAS, REASON_RAUTO };
enum { KIND_SCOOTER = 0, KIND_POLICE };
enum { MSG_NONE = 0, MSG_ITEM, MSG_GAS, MSG_ALL, MSG_LOW, MSG_HIT, MSG_NEED, MSG_RAUTI, MSG_BOOM };

#define MAX_SPEED_Q4 48
#define ACCEL_Q4 3
#define BRAKE_Q4 8
#define FRICTION_Q4 2
#define TURN_STEPS 3    /* 32nds of a turn per tick: a right angle in three ticks */
#define NUDGE_REACH 7   /* how far off a street opening the car is still steered into it */

#define FUEL_MAX 3600
#define FUEL_REFILL_RATE 40
#define TROUBLE_MAX 3
#define INVULN_TICKS 60
#define STALL_TICKS 90
#define HONK_COOLDOWN 45
#define HONK_RADIUS 80
#define SCARED_TICKS 100
#define PATIENCE_TICKS 600 /* a chaser that has not caught the car in 20 s gives up */

/* Rauti: bangers. A box holds twenty; B drops one behind the car and it goes
 * off two seconds later, taking out any chaser nearby -- and denting the
 * Fiat if it has not driven clear. */
#define RAUTI_PER_BOX 20
#define RAUTO_FUSE_TICKS 61 /* 2 s of 33 ms steps */
#define RAUTO_RADIUS 30     /* world pixels */
#define RAUTO_COOLDOWN 10
#define RAUTO_BOOM_TICKS 10
#define MAX_RAUTI 4         /* lit at the same time */

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
    int16_t x, y;       /* world pixels */
    uint8_t fuse, boom; /* ticks to the bang; then ticks of blast left to draw */
} duke_rauto_t;

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
static duke_rauto_t s_lit[MAX_RAUTI];
static uint8_t s_box_taken[CM_RAUTI_COUNT];
static int s_rauti, s_rauto_cd;
static int s_poi = -1, s_poi_ticks; /* the point of interest the car is at, and since when */
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

static const char *poi_name(int i) {
    switch (i) {
    case CM_AT_PLEBISCITO: return "PIAZZA DEL PLEBISCITO";
    case CM_AT_SAN_FRANCESCO: return "SAN FRANCESCO DI PAOLA";
    case CM_AT_PALAZZO_REALE: return "PALAZZO REALE";
    case CM_AT_MASCHIO_ANGIOINO: return "MASCHIO ANGIOINO";
    case CM_AT_CASTEL_DELL_OVO: return "CASTEL DELL'OVO";
    case CM_AT_SANT_ELMO: return "CASTEL SANT'ELMO";
    case CM_AT_SAN_PAOLO: return "STADIO SAN PAOLO";
    case CM_AT_DUOMO: return "DUOMO";
    case CM_AT_MUSEO: return "MUSEO NAZIONALE";
    case CM_AT_CAPODIMONTE: return "REGGIA DI CAPODIMONTE";
    case CM_AT_VILLA_DORIA: return "VILLA DORIA D'ANGRI";
    case CM_AT_STAZIONE: return "STAZIONE CENTRALE";
    case CM_AT_GALLERIA: return "GALLERIA UMBERTO I";
    case CM_AT_CENTRO_DIREZIONALE: return "CENTRO DIREZIONALE";
    case CM_AT_MOSTRA: return "MOSTRA D'OLTREMARE";
    case CM_AT_CAPODICHINO: return "AEROPORTO DI CAPODICHINO";
    case CM_AT_VILLA_COMUNALE: return "VILLA COMUNALE";
    case CM_AT_BEVERELLO: return "MOLO BEVERELLO";
    case CM_AT_DANTE: return "PIAZZA DANTE";
    case CM_AT_VANVITELLI: return "PIAZZA VANVITELLI";
    case CM_AT_MERCATO: return "PIAZZA MERCATO";
    default: return "PIAZZA DEI MARTIRI";
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

/* The camera is in screen pixels (world pixels times ZOOM), taken from the
 * car's Q4 position so the scroll is smooth to the screen pixel. */
static void update_camera(void) {
    s_camera_x = duke_clamp((int)(s_player.x_q4 * ZOOM / Q4) - PRG32_GAME_W / 2, 0,
                            WORLD_PX_W * ZOOM - PRG32_GAME_W);
    s_camera_y = duke_clamp((int)(s_player.y_q4 * ZOOM / Q4) - PRG32_GAME_H / 2, 0,
                            WORLD_PX_H * ZOOM - PRG32_GAME_H);
}

/* Where a world pixel is on screen, for panning effects. */
static int screen_x(int world_x) { return world_x * ZOOM - s_camera_x; }

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
    for (int i = 0; i < MAX_RAUTI; ++i) s_lit[i].fuse = s_lit[i].boom = 0;
    for (int i = 0; i < CM_RAUTI_COUNT; ++i) s_box_taken[i] = 0;
    s_rauti = s_rauto_cd = 0;
    s_poi = -1;
    s_poi_ticks = 0;
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
/* The D-pad points where the car should go: it turns that way by the
 * shortest side and accelerates by itself; let go and it coasts to a stop.
 * Returns the wanted heading (0..31), or -1 with the pad released. */
static int wanted_heading(uint32_t input) {
    int dx = ((input & PRG32_BTN_RIGHT) ? 1 : 0) - ((input & PRG32_BTN_LEFT) ? 1 : 0);
    int dy = ((input & PRG32_BTN_DOWN) ? 1 : 0) - ((input & PRG32_BTN_UP) ? 1 : 0);
    if (dx == 0 && dy == 0) return -1;
    if (dx == 0) return dy < 0 ? 0 : 16;
    if (dy == 0) return dx > 0 ? 8 : 24;
    if (dx > 0) return dy < 0 ? 4 : 12;
    return dy < 0 ? 28 : 20;
}

/* The car is pushed along one axis but something is in the way: if a street
 * opens within NUDGE_REACH pixels to either side, slide one pixel towards
 * it, so a turn taken a little early or late still goes in. */
static void nudge_into_opening(int step_x, int step_y) {
    int px = player_x(), py = player_y();
    for (int reach = 1; reach <= NUDGE_REACH; ++reach) {
        for (int side = -1; side <= 1; side += 2) {
            int ox = step_y ? side * reach : 0, oy = step_x ? side * reach : 0;
            if (cm_box_blocked(px + ox + step_x, py + oy + step_y)) continue;
            /* Every pixel on the way there must be free as well. */
            int sx = step_y ? side : 0, sy = step_x ? side : 0;
            if (cm_box_blocked(px + sx, py + sy)) continue;
            s_player.x_q4 += sx * Q4;
            s_player.y_q4 += sy * Q4;
            return;
        }
    }
}

static void update_player(uint32_t input) {
    duke_car_t *c = &s_player;
    int want = s_fuel > 0 ? wanted_heading(input) : -1;

    if (want >= 0) {
        int diff = (want - c->heading) & 31;
        int off = diff <= 16 ? diff : 32 - diff;
        int turn = duke_min(off, TURN_STEPS);
        c->heading = (uint8_t)((c->heading + (diff <= 16 ? turn : 32 - turn)) & 31);
        /* Facing the wrong way the car brakes while it comes round, so a
         * U-turn stays inside the street; otherwise it pulls away. */
        if (off > 8) c->speed_q4 = (int16_t)duke_max(0, c->speed_q4 - BRAKE_Q4);
        else c->speed_q4 = (int16_t)duke_min(c->speed_q4 + ACCEL_Q4, MAX_SPEED_Q4);
    } else {
        c->speed_q4 = (int16_t)duke_max(0, c->speed_q4 - FRICTION_Q4);
    }

    int dx = (cm_cos_table[c->heading] * c->speed_q4) / 256;
    int dy = (cm_sin_table[c->heading] * c->speed_q4) / 256;
    int blocked_x = 0, blocked_y = 0;

    /* Axis-separated slide: motion accumulates in Q4, solidity is tested on
     * whole pixels straight from the procedural map. */
    if (!cm_box_blocked((int)((c->x_q4 + dx) / Q4), player_y())) c->x_q4 += dx;
    else blocked_x = 1;
    if (!cm_box_blocked(player_x(), (int)((c->y_q4 + dy) / Q4))) c->y_q4 += dy;
    else blocked_y = 1;
    if (want >= 0 && (want & 7) == 0) {
        if (blocked_x && dy == 0) nudge_into_opening(dx > 0 ? 1 : -1, 0);
        if (blocked_y && dx == 0) nudge_into_opening(0, dy > 0 ? 1 : -1);
    }
    if ((blocked_x || blocked_y) && c->speed_q4 > 8) {
        if (c->speed_q4 > 28) {
            burst(player_x(), player_y(), 4, IX_SPARK);
            sfx(CH_NOISE, I_NOISE, 40, 120, 160, 3);
        }
        c->speed_q4 = (int16_t)(c->speed_q4 * 3 / 4);
    }

    /* Exhaust while accelerating. */
    if (want >= 0 && (s_tick & 3u) == 0) {
        particle(player_x() - cm_cos_table[c->heading] * 7 / 256,
                 player_y() - cm_sin_table[c->heading] * 7 / 256,
                 (int)(rnd() & 3u) - 1, (int)(rnd() & 3u) - 1, 12, IX_SMOKE);
    }

    /* Fuel: an idle drain plus a speed-proportional term. */
    if (s_fuel > 0) {
        s_fuel = duke_max(0, s_fuel - 1 - (c->speed_q4 >> 5));
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
    int note = 31 + c->speed_q4 / 5;
    if (note != s_engine_note) {
        s_engine_note = note;
        sfx(CH_ENGINE, I_ENGINE, note, 96, 160, 0);
    }
}

/* The Fiat takes a knock: from a chaser, or from its own rauto. Returns 1
 * if that was one knock too many and the night is over. */
static int trouble(int reason, int world_x, int world_y) {
    s_trouble++;
    s_invuln = INVULN_TICKS;
    s_shake = 12;
    s_flash = 6;
    s_player.speed_q4 = (int16_t)(s_player.speed_q4 / 3);
    burst(world_x, world_y, 10, IX_SPARK);
    if (s_trouble < TROUBLE_MAX) return 0;
    s_lose_reason = reason;
    finish(ST_LOSE);
    return 1;
}

/* ---- rauti --------------------------------------------------------------
 */
static void update_rauti(uint32_t pressed) {
    int px = player_x(), py = player_y();

    for (int i = 0; i < CM_RAUTI_COUNT; ++i) {
        if (s_box_taken[i]) continue;
        int bx = cm_rauti_points[i].x * CM_TILE_PX + 4, by = cm_rauti_points[i].y * CM_TILE_PX + 4;
        if (duke_abs(px - bx) < 9 && duke_abs(py - by) < 9) {
            s_box_taken[i] = 1;
            s_rauti += RAUTI_PER_BOX;
            s_score += 100;
            burst(bx, by, 8, IX_SPARK);
            sfx(CH_FX, I_CHIME, 67, 210, screen_x(bx), 6);
            say(MSG_RAUTI, 0, 75);
        }
    }

    if (s_rauto_cd > 0) s_rauto_cd--;
    if ((pressed & PRG32_BTN_B) && s_rauti > 0 && s_rauto_cd == 0) {
        for (int i = 0; i < MAX_RAUTI; ++i) {
            duke_rauto_t *r = &s_lit[i];
            if (r->fuse || r->boom) continue;
            /* Dropped where the car is; the car drives off it. */
            r->x = (int16_t)px;
            r->y = (int16_t)py;
            r->fuse = RAUTO_FUSE_TICKS;
            s_rauti--;
            s_rauto_cd = RAUTO_COOLDOWN;
            sfx(CH_FX, I_BLIP, 48, 160, 160, 3);
            break;
        }
    }

    for (int i = 0; i < MAX_RAUTI; ++i) {
        duke_rauto_t *r = &s_lit[i];
        if (r->boom) r->boom--;
        if (!r->fuse) continue;
        if ((r->fuse & 7u) == 0) /* the fuse spits sparks */
            particle(r->x, r->y - 3, (int)(rnd() & 7u) - 3, -(int)(rnd() & 3u) - 1, 8, IX_SPARK);
        if (--r->fuse) continue;

        /* The bang. */
        r->boom = RAUTO_BOOM_TICKS;
        s_shake = duke_max(s_shake, 8);
        for (int k = 0; k < 14; ++k) {
            particle(r->x, r->y, cm_cos_table[(k * 7) & 31] / 24, cm_sin_table[(k * 7) & 31] / 24,
                     10 + (k & 3), (k & 1) ? IX_SPARK : IX_SMOKE);
        }
        sfx(CH_NOISE, I_NOISE, 26, 250, screen_x(r->x), 12);
        for (int e = 0; e < MAX_ENEMIES; ++e) {
            duke_enemy_t *en = &s_enemies[e];
            if (!en->active) continue;
            if (duke_abs(en->x - r->x) < RAUTO_RADIUS && duke_abs(en->y - r->y) < RAUTO_RADIUS) {
                en->active = 0;
                s_score += 150;
                burst(en->x, en->y, 8, IX_SPARK);
            }
        }
        if (duke_abs(px - r->x) < RAUTO_RADIUS && duke_abs(py - r->y) < RAUTO_RADIUS && s_invuln == 0) {
            say(MSG_BOOM, 0, 60);
            if (trouble(REASON_RAUTO, px, py)) return;
        }
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
        int dx = (int)(rnd() % 45u) - 22, dy = (int)(rnd() % 45u) - 22;
        if (duke_abs(dx) < 12 && duke_abs(dy) < 8) continue; /* off screen only */
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
        if (dist > 220 || --e->patience == 0) { e->active = 0; continue; }
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
        if (e->kind == KIND_POLICE && dist < 110) siren_x = duke_clamp(screen_x(e->x), 0, 319);

        if (s_invuln == 0 && dist < 2 * CM_CAR_HALF) {
            e->active = 0;
            sfx(CH_NOISE, I_NOISE, 30, 230, screen_x(e->x), 8);
            say(MSG_HIT, e->kind, 60);
            if (trouble(e->kind == KIND_SCOOTER ? REASON_SCOOTER : REASON_POLICE, e->x, e->y)) return;
        }
    }

    /* A two-tone siren from wherever the nearest police car is. */
    if (siren_x >= 0 && (s_tick & 7u) == 0 && s_sfx_left[CH_NOISE - CH_ENGINE] == 0)
        sfx(CH_NOISE, I_SIREN, (s_tick & 8u) ? 76 : 71, 120, siren_x, 6);

    /* The band plays faster when someone is on the Fiat's tail. */
    s_danger = nearest < 80;
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
            sfx(CH_FX, I_CHIME, 72 + s_collected_count * 2, 210, screen_x(ix), 6);
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
        if (s_state == ST_PLAYING) update_rauti(pressed);
        if (s_state == ST_PLAYING) update_enemies(pressed);
        if (s_state == ST_PLAYING) update_items_and_party();
        update_particles();
        update_camera();
        {
            int poi = cm_poi_near(player_x() >> 3, player_y() >> 3);
            if (poi != s_poi) { s_poi = poi; s_poi_ticks = 0; }
            else if (s_poi_ticks < 255) s_poi_ticks++;
        }
        break;
    case ST_PAUSED:
        if (pressed & PRG32_BTN_START) s_state = ST_PLAYING;
        break;
    case ST_WIN:
        if ((s_tick & 3u) == 0) {
            /* Confetti over the villa. */
            uint32_t r = rnd();
            particle((s_camera_x + (int)(r % 320u)) / ZOOM, s_camera_y / ZOOM, (int)((r >> 9) & 3u) - 1, 4,
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

/* A square one-bit icon (8x8 or 16x16) with a transparent background. */
static void icon_sized(const uint8_t *bits, int size, uint16_t colour, int x, int y) {
    uint16_t pal[2];
    prg32_indexed_sprite_t s;
    pal[0] = 0; pal[1] = colour;
    s.pixels = bits; s.palette = pal;
    s.width = (uint16_t)size; s.height = (uint16_t)size; s.frame_count = 1; s.palette_count = 2;
    s.bits_per_pixel = PRG32_SPRITE_BPP_1;
    s.transparent_index = 0;
    prg32_sprite_draw_indexed(x, y, &s, 0);
}

static void icon(const uint8_t *bits, uint16_t colour, int x, int y) {
    icon_sized(bits, 8, colour, x, y);
}

static void icon16(const uint8_t *bits, uint16_t colour, int x, int y) {
    icon_sized(bits, 16, colour, x, y);
}

static void text(int x, int y, const char *s, uint16_t fg) {
    prg32_gfx_text8(x, y, s, fg, PRG32_COLOR_BLACK);
}

static void text_centred(int y, const char *s, uint16_t fg) {
    text((PRG32_GAME_W - duke_strlen(s) * 8) / 2, y, s, fg);
}

/* The city, drawn from the procedural map every frame: one indexed rectangle
 * per run of equal ground along a row, then the small details. */
#define VIEW_COLS (PRG32_GAME_W / TILE_SCREEN + 1)
#define VIEW_ROWS (PRG32_GAME_H / TILE_SCREEN + 2)
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
    case CM_T_LANDMARK: return IX_KERB;
    default: return IX_ASPHALT;
    }
}

/* `cam_x`, `cam_y`: the screen-pixel position of the viewport in the zoomed
 * world. Details are given in world pixels inside the tile and zoomed. */
static void detail(int x, int y, int ox, int oy, int w, int h, int index) {
    box(x + ox * ZOOM, y + oy * ZOOM, w * ZOOM, h * ZOOM, index);
}

static void draw_city(int cam_x, int cam_y) {
    int tx0 = cam_x >= 0 ? cam_x / TILE_SCREEN : -((-cam_x + TILE_SCREEN - 1) / TILE_SCREEN);
    int ty0 = cam_y >= 0 ? cam_y / TILE_SCREEN : -((-cam_y + TILE_SCREEN - 1) / TILE_SCREEN);
    int ox = tx0 * TILE_SCREEN - cam_x, oy = ty0 * TILE_SCREEN - cam_y;

    for (int gy = 0; gy < VIEW_ROWS + 2; ++gy)
        for (int gx = 0; gx < VIEW_COLS + 2; ++gx)
            s_grid[gy][gx] = cm_tile_at(tx0 - 1 + gx, ty0 - 1 + gy);

    for (int gy = 1; gy <= VIEW_ROWS; ++gy) {
        int ty = ty0 - 1 + gy, y = oy + (gy - 1) * TILE_SCREEN;
        int run_start = 1, run_index = ground_index(s_grid[gy][1], tx0, ty);
        for (int gx = 2; gx <= VIEW_COLS + 1; ++gx) {
            int index = gx <= VIEW_COLS ? ground_index(s_grid[gy][gx], tx0 - 1 + gx, ty) : -1;
            if (index == run_index) continue;
            box(ox + (run_start - 1) * TILE_SCREEN, y, (gx - run_start) * TILE_SCREEN, TILE_SCREEN,
                run_index);
            run_start = gx;
            run_index = index;
        }
    }

    for (int gy = 1; gy <= VIEW_ROWS; ++gy) {
        int ty = ty0 - 1 + gy, y = oy + (gy - 1) * TILE_SCREEN;
        for (int gx = 1; gx <= VIEW_COLS; ++gx) {
            int tx = tx0 - 1 + gx, x = ox + (gx - 1) * TILE_SCREEN;
            uint32_t h;
            switch (s_grid[gy][gx]) {
            case CM_T_BUILDING: {
                /* Roofs are lit from the north-west: a bright rim there, the
                 * shaded wall showing along the south and east. */
                int edge = 0;
                if (s_grid[gy - 1][gx] != CM_T_BUILDING) { detail(x, y, 0, 0, 8, 1, IX_LIGHT); edge = 1; }
                if (s_grid[gy][gx - 1] != CM_T_BUILDING) { detail(x, y, 0, 0, 1, 8, IX_LIGHT); edge = 1; }
                if (s_grid[gy + 1][gx] != CM_T_BUILDING) { detail(x, y, 0, 5, 8, 3, IX_SHADOW); edge = 1; }
                if (s_grid[gy][gx + 1] != CM_T_BUILDING) { detail(x, y, 6, 0, 2, 8, IX_SHADOW); edge = 1; }
                h = cm_hash(tx, ty);
                if (!edge && (h & 3u) == 0) {
                    /* A skylight, with its frame now that there is room for one. */
                    detail(x, y, 2, 2, 3, 3, IX_SHADOW);
                    box(x + 5, y + 5, 4, 4, (h & 4u) ? IX_WINDOW : IX_WINDOW2);
                } else if (!edge && (h & 15u) == 5) {
                    box(x + 6, y + 4, 3, 8, IX_SHADOW); /* a chimney and its shadow */
                    box(x + 6, y + 4, 3, 3, IX_LIGHT);
                }
                break;
            }
            case CM_T_PARK:
                if ((tx + ty) & 1) {
                    detail(x, y, 1, 1, 5, 5, IX_TREE);
                    box(x + 4, y + 4, 4, 4, IX_PARK); /* light on the crown */
                } else {
                    detail(x, y, 4, 4, 3, 3, IX_TREE);
                }
                break;
            case CM_T_PIAZZA:
                if (((tx + ty) & 1) == 0) detail(x, y, 3, 3, 2, 2, IX_PAVE);
                break;
            case CM_T_ROAD_LINE:
                detail(x, y, 1, 3, 6, 2, IX_LINE);
                break;
            case CM_T_PROMENADE:
                /* A white parapet wherever the lungomare meets the water. */
                if (s_grid[gy + 1][gx] == CM_T_SEA) detail(x, y, 0, 6, 8, 2, IX_RAIL);
                if (s_grid[gy - 1][gx] == CM_T_SEA) detail(x, y, 0, 0, 8, 2, IX_RAIL);
                if (s_grid[gy][gx + 1] == CM_T_SEA) detail(x, y, 6, 0, 2, 8, IX_RAIL);
                if (s_grid[gy][gx - 1] == CM_T_SEA) detail(x, y, 0, 0, 2, 8, IX_RAIL);
                break;
            case CM_T_SEA:
                h = cm_hash(tx, ty);
                if ((h & 1u) == 0)
                    box(x + (int)((h >> 4) & 7u), y + (int)((h >> 7) & 15u), 6, 1,
                        (h & 256u) ? IX_SEA1 : IX_SEA2);
                break;
            default:
                break;
            }
        }
    }
}

/* ---- the monuments ------------------------------------------------------
 * Each stands on its rectangle of the map and is drawn from a few dozen
 * indexed rectangles, in world pixels from its top-left corner. */
static int s_lm_x, s_lm_y;

static void lm(int ox, int oy, int w, int h, int index) {
    box(s_lm_x + ox * ZOOM, s_lm_y + oy * ZOOM, w * ZOOM, h * ZOOM, index);
}

/* A block of building: lit along the north and west, in shade south and east. */
static void slab(int ox, int oy, int w, int h, int index) {
    lm(ox, oy, w, h, index);
    lm(ox, oy, w, 1, IX_LIGHT);
    lm(ox, oy, 1, h, IX_LIGHT);
    lm(ox, oy + h - 2, w, 2, IX_SHADOW);
    lm(ox + w - 1, oy, 1, h, IX_SHADOW);
}

static void tower(int ox, int oy, int size) {
    slab(ox, oy, size, size, IX_STONE);
    lm(ox + 2, oy + 2, size - 4, size - 5, IX_SHADOW);
    lm(ox + 3, oy + 3, size - 6, size - 7, IX_STONE);
}

static void draw_monument(int style, int w, int h) {
    switch (style) {
    case CM_POI_CASTLE: /* Maschio Angioino: five round towers, the marble arch between two */
        slab(3, 5, w - 6, h - 10, IX_STONE);
        lm(10, 13, w - 20, h - 26, IX_SHADOW);
        lm(11, 14, w - 22, h - 28, IX_PAVE);
        tower(0, 1, 10); tower(w - 10, 1, 10);
        tower(0, h - 11, 10); tower(w / 2 - 5, h - 11, 10); tower(w - 10, h - 11, 10);
        lm(10, h - 9, w / 2 - 15, 8, IX_MARBLE);
        break;
    case CM_POI_STAR_FORT: /* Castel Sant'Elmo: a six-pointed star on the hill */
        slab(7, 7, w - 14, h - 14, IX_STONE);
        slab(w / 2 - 5, 0, 10, 9, IX_STONE); slab(w / 2 - 5, h - 9, 10, 9, IX_STONE);
        slab(0, 9, 9, 9, IX_STONE); slab(0, h - 18, 9, 9, IX_STONE);
        slab(w - 9, 9, 9, 9, IX_STONE); slab(w - 9, h - 18, 9, 9, IX_STONE);
        lm(13, 13, w - 26, h - 26, IX_SHADOW);
        lm(14, 14, w - 28, h - 28, IX_PAVE);
        break;
    case CM_POI_SEA_CASTLE: /* Castel dell'Ovo: a long keep on the rock */
        slab(0, 5, w, h - 6, IX_STONE);
        slab(6, 0, 20, 12, IX_STONE);
        slab(34, 2, 24, 10, IX_STONE);
        for (int i = 2; i < w - 2; i += 6) lm(i, h - 6, 2, 2, IX_SHADOW);
        lm(10, 3, 12, 5, IX_SHADOW);
        break;
    case CM_POI_PALACE: /* Palazzo Reale, the Reggia: red fronts, courtyards */
        slab(0, 0, w, h, IX_REDWALL);
        if (h > w) {
            lm(8, h / 4 - 6, w - 16, 12, IX_PAVE);
            lm(8, 3 * h / 4 - 6, w - 16, 12, IX_PAVE);
            for (int i = 3; i < h - 3; i += 4) { lm(2, i, 2, 2, IX_MARBLE); lm(w - 5, i, 2, 2, IX_MARBLE); }
        } else {
            lm(6, 5, w / 2 - 10, h - 10, IX_PAVE);
            lm(w / 2 + 4, 5, w / 2 - 10, h - 10, IX_PAVE);
            for (int i = 3; i < w - 3; i += 4) lm(i, h - 5, 2, 2, IX_MARBLE);
        }
        break;
    case CM_POI_BASILICA: /* San Francesco di Paola: the dome between its colonnades */
        lm(8, 0, 6, h, IX_MARBLE);
        for (int i = 1; i < h - 1; i += 4) lm(9, i, 2, 2, IX_SHADOW);
        slab(0, h / 2 - 8, 16, 16, IX_STONE);
        lm(2, h / 2 - 6, 12, 12, IX_COPPER);
        lm(4, h / 2 - 4, 8, 8, IX_MARBLE);
        lm(6, h / 2 - 2, 4, 4, IX_COPPER);
        lm(7, h / 2 - 1, 2, 2, IX_LIGHT);
        break;
    case CM_POI_CATHEDRAL: /* the Duomo: a Latin cross, a dome on the crossing, the bell tower */
        slab(11, 1, 10, h - 2, IX_STONE);
        slab(3, 8, 26, 9, IX_STONE);
        lm(15, 2, 2, h - 5, IX_LIGHT);
        lm(12, 8, 8, 8, IX_COPPER);
        lm(14, 10, 4, 4, IX_MARBLE);
        slab(23, 21, 8, 9, IX_MARBLE);
        lm(11, h - 6, 10, 4, IX_MARBLE);
        break;
    case CM_POI_MUSEUM: /* Museo Archeologico Nazionale */
        slab(0, 0, w, h, IX_REDWALL);
        lm(4, 7, 9, h - 14, IX_PAVE);
        lm(w - 13, 7, 9, h - 14, IX_PAVE);
        lm(14, 2, 4, h - 6, IX_MARBLE);
        for (int i = 3; i < w - 3; i += 4) lm(i, h - 5, 2, 2, IX_MARBLE);
        break;
    case CM_POI_STATION: /* Stazione Centrale: the tracks come in from the north */
        for (int i = 0; i < 6; ++i) {
            lm(4 + i * 10, 0, 1, 14, IX_TRACK);
            lm(7 + i * 10, 0, 1, 14, IX_TRACK);
            lm(9 + i * 10, 2, 3, 12, IX_PAVE);
        }
        slab(0, 13, w, h - 13, IX_MARBLE);
        for (int i = 4; i < w - 2; i += 8) lm(i, 15, 1, h - 18, IX_SHADOW);
        lm(2, 21, w - 4, 1, IX_SHADOW);
        break;
    case CM_POI_GALLERIA: /* Galleria Umberto I: a glass cross and its dome */
        slab(0, 0, w, h, IX_STONE);
        lm(w / 2 - 2, 1, 4, h - 3, IX_GLASS);
        lm(1, h / 2 - 2, w - 2, 4, IX_GLASS);
        lm(w / 2 - 5, h / 2 - 5, 10, 10, IX_GLASS);
        lm(w / 2 - 2, h / 2 - 2, 4, 4, IX_LIGHT);
        break;
    case CM_POI_TOWERS: /* the Centro Direzionale, brand new in 1997 */
        lm(0, 0, w, h, IX_PAVE);
        lm(14, 6, 4, 16, IX_SHADOW); slab(2, 2, 12, 16, IX_GLASS);
        lm(28, 10, 4, 22, IX_SHADOW); slab(18, 6, 10, 22, IX_GLASS);
        lm(44, 6, 3, 14, IX_SHADOW); slab(32, 2, 12, 14, IX_GLASS);
        lm(20, 32, 4, 14, IX_SHADOW); slab(6, 28, 14, 16, IX_GLASS);
        lm(42, 30, 4, 16, IX_SHADOW); slab(28, 26, 14, 18, IX_GLASS);
        lm(4, 4, 2, 10, IX_LIGHT); lm(20, 8, 2, 16, IX_LIGHT); lm(30, 28, 2, 12, IX_LIGHT);
        break;
    case CM_POI_FAIR: /* Mostra d'Oltremare: the gardens and the fountain of the Esedra */
        lm(0, 0, w, h, IX_PARK);
        lm(18, 12, 28, 24, IX_PAVE);
        lm(20, 14, 24, 20, IX_SEA1);
        for (int i = 0; i < 5; ++i) lm(23 + i * 4, 22, 2, 4, IX_LIGHT);
        slab(2, 2, 14, 10, IX_MARBLE);
        slab(w - 16, h - 14, 14, 12, IX_MARBLE);
        for (int i = 4; i < w - 4; i += 9) { lm(i, h - 6, 4, 4, IX_TREE); lm(i + 3, 15, 4, 4, IX_TREE); }
        break;
    default:
        break;
    }
}

static void draw_monuments(int cam_x, int cam_y) {
    for (int i = 0; i < CM_POI_COUNT; ++i) {
        const cm_poi_t *p = &cm_pois[i];
        int w = p->w * CM_TILE_PX, h = p->h * CM_TILE_PX;
        s_lm_x = p->x * TILE_SCREEN - cam_x;
        s_lm_y = p->y * TILE_SCREEN - cam_y;
        if (s_lm_x >= PRG32_GAME_W || s_lm_y >= PRG32_GAME_H || s_lm_x + w * ZOOM <= 0 ||
            s_lm_y + h * ZOOM <= 0)
            continue;
        if (i == CM_AT_SAN_PAOLO) {
            /* The stadium: two tiers of stands around the marked pitch. */
            lm(3, 3, w - 6, 2, IX_SHADOW); lm(3, h - 5, w - 6, 2, IX_SHADOW);
            lm(3, 3, 2, h - 6, IX_SHADOW); lm(w - 5, 3, 2, h - 6, IX_SHADOW);
            lm(12, 12, w - 24, 2, IX_KERB); lm(12, h - 14, w - 24, 2, IX_KERB);
            lm(12, 12, 2, h - 24, IX_KERB); lm(w - 14, 12, 2, h - 24, IX_KERB);
            lm(25, 25, 46, 1, IX_RAIL); lm(25, 54, 46, 1, IX_RAIL);
            lm(25, 25, 1, 30, IX_RAIL); lm(70, 25, 1, 30, IX_RAIL);
            lm(47, 25, 1, 30, IX_RAIL); lm(44, 37, 7, 6, IX_RAIL); lm(45, 38, 5, 4, IX_PARK);
        } else if (i == CM_AT_PLEBISCITO) {
            /* The two bronze horsemen in front of the basilica. */
            lm(12, 16, 3, 3, IX_SHADOW); lm(12, 37, 3, 3, IX_SHADOW);
        }
        draw_monument(p->style, w, h);
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

/* A pick-up on the ground: a pulsing marker, a black badge, the icon bobbing. */
static void draw_pickup(const uint8_t *bits16, uint16_t colour, int x, int y) {
    static const int8_t bob[8] = {0, -1, -2, -3, -2, -1, 0, 1};
    int r = 12 + (int)((s_tick >> 2) & 3u) * 2, lift = bob[(s_tick >> 2) & 7u];
    box(x + 8 - r, y + 8 - r, 3, 3, IX_SPARK);
    box(x + 5 + r, y + 8 - r, 3, 3, IX_SPARK);
    box(x + 8 - r, y + 5 + r, 3, 3, IX_SPARK);
    box(x + 5 + r, y + 5 + r, 3, 3, IX_SPARK);
    box(x - 2, y - 2 + lift, 20, 20, IX_BLACK);
    icon16(bits16, colour, x, y + lift);
}

static void draw_world(void) {
    int cam_x = s_camera_x, cam_y = s_camera_y;
    if (s_shake) {
        cam_x += ((int)(s_tick & 2u) - 1) * ZOOM;
        cam_y += ((int)((s_tick >> 1) & 2u) - 1) * ZOOM;
    }
    draw_city(cam_x, cam_y);
    draw_monuments(cam_x, cam_y);

    sprite4(duke_villa_pixels, w_villa, DUKE_VILLA_COLOURS, DUKE_VILLA_WIDTH, DUKE_VILLA_HEIGHT, 1, 0,
            cm_party_point.x * TILE_SCREEN - DUKE_VILLA_WIDTH / 2 - cam_x,
            cm_party_point.y * TILE_SCREEN - 8 - DUKE_VILLA_HEIGHT - cam_y);

    for (int i = 0; i < CM_GAS_COUNT; ++i)
        icon16(duke_gas_icon16, w_gas, cm_gas_points[i].x * TILE_SCREEN - cam_x,
               cm_gas_points[i].y * TILE_SCREEN - cam_y);

    for (int i = 0; i < CM_RAUTI_COUNT; ++i)
        if (!s_box_taken[i])
            draw_pickup(duke_rauti_icon16, w_rauti, cm_rauti_points[i].x * TILE_SCREEN - cam_x,
                        cm_rauti_points[i].y * TILE_SCREEN - cam_y);
    for (int i = 0; i < CM_ITEM_COUNT; ++i)
        if (!s_collected[i])
            draw_pickup(duke_item_icons16[i], w_items[i], cm_item_points[i].x * TILE_SCREEN - cam_x,
                        cm_item_points[i].y * TILE_SCREEN - cam_y);

    /* Lit rauti: a red stick whose fuse blinks faster as it burns down; then
     * the blast, a ring that grows to the radius it reaches. */
    for (int i = 0; i < MAX_RAUTI; ++i) {
        const duke_rauto_t *r = &s_lit[i];
        int x = r->x * ZOOM - cam_x, y = r->y * ZOOM - cam_y;
        if (r->fuse) {
            box(x - 2, y - 4, 5, 9, IX_RED);
            box(x - 2, y - 1, 5, 2, IX_WHITE);
            if (r->fuse > 20 ? (r->fuse & 4u) : (r->fuse & 1u)) box(x - 1, y - 8, 3, 4, IX_SPARK);
        } else if (r->boom) {
            int reach = RAUTO_RADIUS * ZOOM * (RAUTO_BOOM_TICKS + 1 - r->boom) / RAUTO_BOOM_TICKS;
            int core = (int)r->boom * 2, flare = (r->boom & 1u) ? IX_WHITE : IX_SPARK;
            /* The shock front: the corners of the square the blast reaches. */
            for (int k = 0; k < 4; ++k) {
                int cx = x + ((k & 1) ? reach - 8 : -reach), cy = y + ((k & 2) ? reach - 3 : -reach);
                box(cx, cy, 8, 3, IX_SPARK);
                box(x + ((k & 1) ? reach - 3 : -reach), y + ((k & 2) ? reach - 8 : -reach), 3, 8, IX_SPARK);
            }
            /* The fireball: two crossed slabs make a rough disc that shrinks. */
            box(x - core, y - core / 2, 2 * core, core, flare);
            box(x - core / 2, y - core, core, 2 * core, flare);
            box(x - core / 2, y - core / 2, core, core, IX_RED);
        }
    }

    for (int i = 0; i < MAX_PARTICLES; ++i) {
        const duke_particle_t *p = &s_particles[i];
        if (p->life) box(p->x_q2 * ZOOM / 4 - cam_x, p->y_q2 * ZOOM / 4 - cam_y, 3, 3, p->ix);
    }

    for (int i = 0; i < MAX_ENEMIES; ++i) {
        const duke_enemy_t *e = &s_enemies[i];
        if (!e->active) continue;
        if (e->kind == KIND_SCOOTER)
            draw_vehicle(duke_scooter_pixels, w_scooter, DUKE_SCOOTER_COLOURS, e->dir * 2,
                         e->x * ZOOM - cam_x, e->y * ZOOM - cam_y);
        else
            draw_vehicle(duke_police_pixels, w_police, DUKE_POLICE_COLOURS, e->dir * 2,
                         e->x * ZOOM - cam_x, e->y * ZOOM - cam_y);
    }

    int psx = (int)(s_player.x_q4 * ZOOM / Q4) - cam_x, psy = (int)(s_player.y_q4 * ZOOM / Q4) - cam_y;
    if (s_invuln == 0 || (s_tick & 2u) == 0)
        draw_vehicle(duke_car_pixels, w_car, DUKE_CAR_COLOURS, (s_player.heading + 2) >> 2, psx, psy);

    /* The compass: an arrow orbiting the car, pointing at what to fetch next
     * while that is off the screen. */
    if (s_state == ST_PLAYING) {
        int wx, wy, what = target(&wx, &wy);
        int dx = wx - player_x(), dy = wy - player_y();
        if (duke_abs(dx) > 70 || duke_abs(dy) > 44) {
            int h8 = heading8_for(dx, dy);
            icon16(duke_arrow_icons16[h8],
                   what == CM_ITEM_COUNT ? PRG32_COLOR_MAGENTA : PRG32_COLOR_YELLOW,
                   psx - 8 + cm_cos_table[h8 * 4] * 34 / 256, psy - 8 + cm_sin_table[h8 * 4] * 34 / 256);
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
        box(190 + i * 9, 3, 7, 6, i < s_trouble ? IX_RED : IX_RADAR);
    /* Rauti in the boot. */
    icon(duke_rauti_icon, s_rauti ? w_rauti : w_dim, 222, 2);
    text(232, 2, duke_utoa((unsigned)duke_min(s_rauti, 999), buf, 3), s_rauti ? PRG32_COLOR_YELLOW : w_dim);
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
    for (int i = 0; i < CM_RAUTI_COUNT; ++i)
        if (!s_box_taken[i])
            box(rx + cm_rauti_points[i].x / 5, ry + cm_rauti_points[i].y / 5, 1, 1, IX_RED);
    for (int i = 0; i < CM_ITEM_COUNT; ++i)
        if (!s_collected[i] && (s_tick & 8u))
            box(rx + cm_item_points[i].x / 5, ry + cm_item_points[i].y / 5, 2, 2, IX_YELLOW);
    box(rx + cm_party_point.x / 5, ry + cm_party_point.y / 5, 2, 2, IX_MAGENTA);
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (s_enemies[i].active && (s_tick & 4u))
            box(rx + s_enemies[i].x / 40, ry + s_enemies[i].y / 40, 2, 2, IX_RED);
    box(rx + player_x() / 40, ry + player_y() / 40, 2, 2, IX_WHITE);

    /* Where the car is: the name of the place, yellow as it comes into view. */
    if (s_poi >= 0 && s_state == ST_PLAYING)
        text(4, 188, poi_name(s_poi), s_poi_ticks < 60 ? PRG32_COLOR_YELLOW : PRG32_COLOR_WHITE);

    if (s_msg_ticks && s_state == ST_PLAYING) {
        switch (s_msg) {
        case MSG_ITEM: text_centred(172, item_name(s_msg_arg), w_items[s_msg_arg]); break;
        case MSG_ALL: text_centred(172, "ALL ABOARD - TO THE VILLA!", PRG32_COLOR_MAGENTA); break;
        case MSG_GAS: text_centred(172, "FILL 'ER UP", PRG32_COLOR_GREEN); break;
        case MSG_LOW: text_centred(172, "LOW ON GAS!", PRG32_COLOR_RED); break;
        case MSG_NEED: text_centred(172, "NO PARTY WITHOUT THE GEAR", PRG32_COLOR_YELLOW); break;
        case MSG_RAUTI: text_centred(172, "A BOX OF RAUTI: B LIGHTS ONE", w_rauti); break;
        case MSG_BOOM: text_centred(172, "TOO CLOSE TO YOUR OWN RAUTO!", PRG32_COLOR_RED); break;
        case MSG_HIT:
            text_centred(172, s_msg_arg == KIND_SCOOTER ? "SCOOTER GANG!" : "POLIZIA!", PRG32_COLOR_RED);
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
    int car_x = (int)((s_tick * 3u) % 640u) - 40;
    draw_vehicle(duke_car_pixels, w_car, DUKE_CAR_COLOURS, 2, car_x, 180);
    draw_vehicle(duke_scooter_pixels, w_scooter, DUKE_SCOOTER_COLOURS, 2, car_x - 70, 186);
    draw_vehicle(duke_police_pixels, w_police, DUKE_POLICE_COLOURS, 2, car_x - 130, 180);

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
        draw_panel("PAUSED", "D-PAD: DRIVE THAT WAY", "A: HORN   B: LIGHT A RAUTO", IX_WHITE,
                   PRG32_COLOR_CYAN);
    } else if (s_state == ST_WIN) {
        draw_panel("PARTY TIME!", "YOU MADE IT TO THE VILLA", 0, IX_PARTY0, PRG32_COLOR_YELLOW);
    } else if (s_state == ST_LOSE) {
        const char *why = "THEY STOLE THE CINQUECENTO!";
        if (s_lose_reason == REASON_POLICE) why = "BUSTED BY THE POLIZIA!";
        if (s_lose_reason == REASON_GAS) why = "RAN DRY IN THE VICOLI!";
        if (s_lose_reason == REASON_RAUTO) why = "BLOWN UP BY YOUR OWN RAUTO!";
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
int duke_test_rauti(void) { return s_rauti; }
void duke_test_set_rauti(int n) { s_rauti = n; }
int duke_test_poi(void) { return s_poi; }
void duke_test_refuel(void) { s_fuel = FUEL_MAX; }
int duke_test_lit(void) {
    int n = 0;
    for (int i = 0; i < MAX_RAUTI; ++i) n += s_lit[i].fuse ? 1 : 0;
    return n;
}
#endif
