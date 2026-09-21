/*
 * DukesOfDuchesca-napoli97 -- a PRG32 cartridge.
 *
 * Top-down car-chase homage to "The Dukes of Hazzard", relocated to Napoli
 * in 1997: drive a pimped-up white Fiat 500 across a Naples-flavoured city,
 * collecting party paraphernalia while dodging scooter gangs and curious
 * police, and keep an eye on the fuel gauge.
 *
 * The cartridge builder accepts a single C translation unit, so the pure
 * map/logic module is pulled in by #include like cartridges/blackjack does
 * with blackjack_rules.c.
 */
#include "prg32.h"
#include "citymap.c"
#include "assets_generated.h"
#include "assets_icons.h"

/* ---- fixed point + small helpers (freestanding: no libc) -------------- */
#define Q4 16

static int duke_abs(int v) { return v < 0 ? -v : v; }
static int duke_min(int a, int b) { return a < b ? a : b; }
static int duke_max(int a, int b) { return a > b ? a : b; }
static int duke_clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static int duke_wrap(int v, int limit) {
    int w = v % limit;
    return w < 0 ? w + limit : w;
}
static int duke_floor_div8(int v) { return v >= 0 ? v >> 3 : -(((-v) + 7) >> 3); }

/* Decimal formatting without libc: writes up to 4 digits + NUL, returns the
 * string (points into a caller-owned small buffer). */
static char *duke_utoa(unsigned v, char *buf, int bufsize) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    int len = n < bufsize - 1 ? n : bufsize - 1;
    for (int i = 0; i < len; ++i) buf[i] = tmp[len - 1 - i];
    buf[len] = 0;
    return buf;
}

/* ---- world <-> screen ---------------------------------------------------
 * World pixel space is CM_WORLD_COLS*CM_TILE_PX by CM_WORLD_ROWS*CM_TILE_PX,
 * far bigger than the 320x200 viewport. The camera tracks the player with
 * clamping at the world edges; screen coordinates are simply world - camera.
 */
#define WORLD_PX_W (CM_WORLD_COLS * CM_TILE_PX)
#define WORLD_PX_H (CM_WORLD_ROWS * CM_TILE_PX)

static int s_camera_x, s_camera_y;

/* ---- streaming the procedural city into the 64x32 physical playfield --
 * The engine's playfield buffer wraps modulo 64 columns / 32 rows
 * (see prg32_playfield_draw in the firmware). We keep a window of
 * STREAM_WIN_COLS x STREAM_WIN_ROWS tiles around the camera populated with
 * the matching world tiles, re-deriving cells from cm_tile_at() as the
 * camera crosses tile boundaries. Gameplay logic (collision, pickups, gas,
 * party check) never reads the streamed buffer -- it calls cm_tile_at()
 * directly, so it is always correct regardless of what has been streamed.
 */
#define STREAM_MARGIN 2
#define STREAM_WIN_COLS (PRG32_GAME_W / CM_TILE_PX + 2 * STREAM_MARGIN) /* 44 */
#define STREAM_WIN_ROWS (PRG32_GAME_H / CM_TILE_PX + 2 * STREAM_MARGIN) /* 29 */

static int s_stream_tx = -30000, s_stream_ty = -30000;

static void stream_put(int world_tx, int world_ty) {
    int cx = duke_wrap(world_tx, PRG32_PLAYFIELD_COLS);
    int cy = duke_wrap(world_ty, PRG32_PLAYFIELD_ROWS);
    prg32_playfield_put(1, (uint8_t)cx, (uint8_t)cy, cm_tile_at(world_tx, world_ty));
}

static void stream_column(int world_tx, int ty0, int ty1) {
    for (int ty = ty0; ty < ty1; ++ty) stream_put(world_tx, ty);
}

static void stream_row(int world_ty, int tx0, int tx1) {
    for (int tx = tx0; tx < tx1; ++tx) stream_put(tx, world_ty);
}

static void stream_full(int origin_tx, int origin_ty) {
    int tx0 = origin_tx - STREAM_MARGIN, tx1 = tx0 + STREAM_WIN_COLS;
    int ty0 = origin_ty - STREAM_MARGIN, ty1 = ty0 + STREAM_WIN_ROWS;
    for (int ty = ty0; ty < ty1; ++ty) stream_row(ty, tx0, tx1);
}

static void stream_to(int origin_tx, int origin_ty) {
    if (s_stream_tx <= -30000 || duke_abs(origin_tx - s_stream_tx) > 8 ||
        duke_abs(origin_ty - s_stream_ty) > 8) {
        stream_full(origin_tx, origin_ty);
        s_stream_tx = origin_tx;
        s_stream_ty = origin_ty;
        return;
    }
    while (s_stream_tx < origin_tx) {
        s_stream_tx++;
        int wtx = s_stream_tx + (STREAM_WIN_COLS - STREAM_MARGIN - 1);
        stream_column(wtx, s_stream_ty - STREAM_MARGIN,
                      s_stream_ty - STREAM_MARGIN + STREAM_WIN_ROWS);
    }
    while (s_stream_tx > origin_tx) {
        s_stream_tx--;
        int wtx = s_stream_tx - STREAM_MARGIN;
        stream_column(wtx, s_stream_ty - STREAM_MARGIN,
                      s_stream_ty - STREAM_MARGIN + STREAM_WIN_ROWS);
    }
    while (s_stream_ty < origin_ty) {
        s_stream_ty++;
        int wty = s_stream_ty + (STREAM_WIN_ROWS - STREAM_MARGIN - 1);
        stream_row(wty, s_stream_tx - STREAM_MARGIN,
                   s_stream_tx - STREAM_MARGIN + STREAM_WIN_COLS);
    }
    while (s_stream_ty > origin_ty) {
        s_stream_ty--;
        int wty = s_stream_ty - STREAM_MARGIN;
        stream_row(wty, s_stream_tx - STREAM_MARGIN,
                   s_stream_tx - STREAM_MARGIN + STREAM_WIN_COLS);
    }
}

/* ---- tile rendering (layer 1: city, layer 0: gulf/Vesuvio backdrop) ---- */
static void define_tiles(void) {
    static const uint8_t blank[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    static const uint8_t road_line[8] = {
        0x00, 0x00, 0x3c, 0x3c, 0x3c, 0x3c, 0x00, 0x00};
    static const uint8_t brick[8] = {
        0xff, 0x81, 0x81, 0xff, 0x81, 0x81, 0xff, 0x81};
    static const uint8_t plaza[8] = {
        0x24, 0x00, 0x81, 0x00, 0x24, 0x00, 0x81, 0x00};
    static const uint8_t leaves[8] = {
        0x22, 0x88, 0x22, 0x00, 0x88, 0x22, 0x88, 0x00};
    static const uint8_t sparkle[8] = {
        0x81, 0x24, 0x18, 0x7e, 0x18, 0x24, 0x81, 0x00};
    static const uint8_t rail[8] = {
        0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00};
    static const uint8_t vesuvio_l[8] = {
        0x00, 0x00, 0x00, 0x01, 0x03, 0x07, 0x1f, 0x7f};
    static const uint8_t vesuvio_r[8] = {
        0x00, 0x08, 0x0c, 0xce, 0xdc, 0xf8, 0xf8, 0xfe};

    uint16_t asphalt = 0x3a67, line = 0xffe0, brickfg = 0x9a45, brickbg = 0xdaf1;
    uint16_t plazafg = 0xdefb, plazabg = 0xff5b, park = 0x1e83, parkfg = 0x3d67;
    uint16_t gas = 0xffe0, gasfg = 0xf800, party = 0xf81f, partyfg = 0xffff;
    uint16_t prom = 0xd6ba, promfg = 0xffff, sea = 0x1176;

    prg32_tile_define(CM_T_SEA, blank, sea, sea);
    prg32_tile_define(CM_T_ROAD, blank, asphalt, asphalt);
    prg32_tile_define(CM_T_ROAD_LINE, road_line, line, asphalt);
    prg32_tile_define(CM_T_BUILDING, brick, brickfg, brickbg);
    prg32_tile_define(CM_T_PIAZZA, plaza, plazafg, plazabg);
    prg32_tile_define(CM_T_PARK, leaves, parkfg, park);
    prg32_tile_define(CM_T_GAS, blank, gasfg, gas);
    prg32_tile_define(CM_T_PARTY, sparkle, partyfg, party);
    prg32_tile_define(CM_T_PROMENADE, rail, promfg, prom);

    /* Layer 0 uses its own tile ids (0..2), defined into the same 256-entry
     * tile table but never overlapping layer 1's ids in practice because
     * the two layers are drawn from independent playfield buffers. */
    prg32_tile_define(64 + CM_B_SEA, blank, 0x2a5c, 0x2a5c);
    prg32_tile_define(64 + CM_B_VESUVIO_L, vesuvio_l, 0x39a7, 0x2a5c);
    prg32_tile_define(64 + CM_B_VESUVIO_R, vesuvio_r, 0x39a7, 0x2a5c);
}

static void fill_backdrop_once(void) {
    prg32_playfield_clear(0, (uint8_t)(64 + CM_B_SEA));
    for (int y = 0; y < PRG32_PLAYFIELD_ROWS; ++y) {
        for (int x = 0; x < PRG32_PLAYFIELD_COLS; ++x) {
            prg32_playfield_put(0, (uint8_t)x, (uint8_t)y,
                                (uint8_t)(64 + cm_backdrop_at(x, y)));
        }
    }
    prg32_playfield_parallax(0, 96, 96);
    prg32_playfield_parallax(1, PRG32_PARALLAX_1X, PRG32_PARALLAX_1X);
}

/* ---- indexed sprite descriptors (built at runtime: portable cartridges
 * must not embed pointers in const-initialized globals, see
 * cartridges/blackjack/game.c's draw_card_back for the same convention). */
static void sprite_from(prg32_indexed_sprite_t *s, const uint8_t *pixels,
                        const uint16_t *palette, uint16_t w, uint16_t h,
                        uint16_t frames, uint16_t palcount, uint8_t bpp) {
    s->pixels = pixels;
    s->palette = palette;
    s->width = w;
    s->height = h;
    s->frame_count = frames;
    s->palette_count = palcount;
    s->bits_per_pixel = bpp;
    s->transparent_index = 0;
}

/* ---- game state ---------------------------------------------------------
 */
enum {
    ST_TITLE = 0,
    ST_PLAYING,
    ST_PAUSED,
    ST_WIN,
    ST_LOSE,
};

enum {
    REASON_NONE = 0,
    REASON_SCOOTER,
    REASON_POLICE,
    REASON_GAS,
};

#define MAX_SPEED_Q4 56
#define MAX_REVERSE_Q4 24
#define ACCEL_Q4 3
#define BRAKE_Q4 6
#define FRICTION_Q4 1
#define TURN_MIN_SPEED_Q4 4
#define CAR_HALF 5

#define FUEL_MAX 3000
#define FUEL_REFILL_RATE 15
#define TROUBLE_MAX 3
#define INVULN_FRAMES 45
#define STALL_FRAMES 90

#define MAX_ENEMIES 5
#define ENEMY_SPAWN_RADIUS_MIN 170
#define ENEMY_SPAWN_RADIUS_MAX 215
#define ENEMY_DESPAWN_RADIUS 320

typedef struct {
    int32_t x_q4, y_q4;
    int16_t speed_q4;
    uint8_t heading; /* 0..31 */
} duke_car_t;

typedef struct {
    int32_t x_q4, y_q4;
    uint8_t kind; /* 0 = scooter, 1 = police */
    uint8_t active;
    uint8_t heading8;
} duke_enemy_t;

static int s_state = ST_TITLE;
static uint32_t s_last_input;
static uint32_t s_frame;

static duke_car_t s_player;
static int s_fuel;
static int s_trouble;
static int s_invuln;
static int s_stall;
static uint8_t s_collected[CM_ITEM_COUNT];
static int s_collected_count;
static int s_gas_tick;
static int s_lose_reason;
static int s_win_time;

static duke_enemy_t s_enemies[MAX_ENEMIES];
static int s_spawn_timer;

static int heading8_for_delta(int dx, int dy) {
    int adx = duke_abs(dx), ady = duke_abs(dy);
    if (adx < 2 && ady < 2) return 4; /* facing "down" as a neutral default */
    if (adx > ady * 2) return dx > 0 ? 2 : 6;       /* E : W */
    if (ady > adx * 2) return dy > 0 ? 4 : 0;       /* S : N */
    if (dx > 0) return dy > 0 ? 3 : 1;              /* SE : NE */
    return dy > 0 ? 5 : 7;                           /* SW : NW */
}

static void reset_game(void) {
    s_player.x_q4 = (int32_t)(cm_start_point.x * CM_TILE_PX + 4) * Q4;
    s_player.y_q4 = (int32_t)(cm_start_point.y * CM_TILE_PX + 4) * Q4;
    s_player.speed_q4 = 0;
    s_player.heading = 0;
    s_fuel = FUEL_MAX;
    s_trouble = 0;
    s_invuln = 0;
    s_stall = 0;
    s_gas_tick = 0;
    s_lose_reason = REASON_NONE;
    s_win_time = 0;
    s_collected_count = 0;
    for (int i = 0; i < CM_ITEM_COUNT; ++i) s_collected[i] = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i) s_enemies[i].active = 0;
    s_spawn_timer = 90;
    s_stream_tx = -30000;
    s_stream_ty = -30000;

    int px = (int)(s_player.x_q4 / Q4);
    int py = (int)(s_player.y_q4 / Q4);
    s_camera_x = duke_clamp(px - PRG32_GAME_W / 2, 0, WORLD_PX_W - PRG32_GAME_W);
    s_camera_y = duke_clamp(py - PRG32_GAME_H / 2, 0, WORLD_PX_H - PRG32_GAME_H);
    prg32_playfield_camera(s_camera_x, s_camera_y);
    stream_to(duke_floor_div8(s_camera_x), duke_floor_div8(s_camera_y));
}

/* Returns 1 if a CAR_HALF-radius box centered at (px,py) overlaps solid
 * ground -- queried straight from the procedural map, independent of the
 * streamed rendering buffer, so it is always correct near the player. */
static int box_blocked(int px, int py) {
    static const int ox[4] = {-CAR_HALF, CAR_HALF, -CAR_HALF, CAR_HALF};
    static const int oy[4] = {-CAR_HALF, -CAR_HALF, CAR_HALF, CAR_HALF};
    for (int i = 0; i < 4; ++i) {
        int tx = (px + ox[i]) >> 3;
        int ty = (py + oy[i]) >> 3;
        if (cm_tile_is_solid(cm_tile_at(tx, ty))) return 1;
    }
    return 0;
}

static void update_player(uint32_t input) {
    if (s_fuel > 0 && (input & PRG32_BTN_UP)) {
        s_player.speed_q4 = (int16_t)duke_min(s_player.speed_q4 + ACCEL_Q4, MAX_SPEED_Q4);
    } else if (input & PRG32_BTN_DOWN) {
        s_player.speed_q4 = (int16_t)duke_max(s_player.speed_q4 - BRAKE_Q4, -MAX_REVERSE_Q4);
    } else {
        if (s_player.speed_q4 > 0)
            s_player.speed_q4 = (int16_t)duke_max(0, s_player.speed_q4 - FRICTION_Q4);
        else if (s_player.speed_q4 < 0)
            s_player.speed_q4 = (int16_t)duke_min(0, s_player.speed_q4 + FRICTION_Q4);
    }
    if (s_fuel <= 0) {
        /* Stalled: coast to a stop, no more throttle. */
        if (s_player.speed_q4 > 0) s_player.speed_q4 = (int16_t)duke_max(0, s_player.speed_q4 - BRAKE_Q4);
        if (s_player.speed_q4 < 0) s_player.speed_q4 = (int16_t)duke_min(0, s_player.speed_q4 + BRAKE_Q4);
    }

    if (duke_abs(s_player.speed_q4) >= TURN_MIN_SPEED_Q4) {
        if (input & PRG32_BTN_LEFT) s_player.heading = (uint8_t)((s_player.heading + 31) & 31);
        if (input & PRG32_BTN_RIGHT) s_player.heading = (uint8_t)((s_player.heading + 1) & 31);
    }

    int dx = (cm_cos_table[s_player.heading] * s_player.speed_q4) >> 8;
    int dy = (cm_sin_table[s_player.heading] * s_player.speed_q4) >> 8;

    int py = (int)(s_player.y_q4 / Q4);
    int blocked_axis = 0;

    /* Axis-separated slide collision, operating on whole-pixel positions
     * for the solidity test but accumulating motion in Q4 for smoothness. */
    int32_t try_x = s_player.x_q4 + dx;
    int new_px = (int)(try_x / Q4);
    if (!box_blocked(new_px, py)) {
        s_player.x_q4 = try_x;
    } else {
        blocked_axis = 1;
    }
    int32_t try_y = s_player.y_q4 + dy;
    int new_py = (int)(try_y / Q4);
    if (!box_blocked((int)(s_player.x_q4 / Q4), new_py)) {
        s_player.y_q4 = try_y;
    } else {
        blocked_axis = 1;
    }
    if (blocked_axis && duke_abs(s_player.speed_q4) > 8) {
        s_player.speed_q4 = (int16_t)(s_player.speed_q4 * 3 / 4);
    }

    /* Fuel: base idle drain plus a speed-proportional term. */
    s_fuel -= 1 + (duke_abs(s_player.speed_q4) >> 5);
    if (s_fuel < 0) s_fuel = 0;

    int ptx = (int)(s_player.x_q4 / Q4) >> 3;
    int pty = (int)(s_player.y_q4 / Q4) >> 3;
    uint8_t under = cm_tile_at(ptx, pty);
    if (under == CM_T_GAS && s_fuel < FUEL_MAX) {
        s_fuel = duke_min(FUEL_MAX, s_fuel + FUEL_REFILL_RATE);
        if (s_gas_tick <= 0) {
            prg32_buzzer_tone(700, 40, 300);
            s_gas_tick = 12;
        }
    }
    if (s_gas_tick > 0) s_gas_tick--;

    if (s_fuel <= 0 && duke_abs(s_player.speed_q4) <= 1) {
        s_stall++;
        if (s_stall > STALL_FRAMES) {
            s_state = ST_LOSE;
            s_lose_reason = REASON_GAS;
        }
    } else {
        s_stall = 0;
    }

    if (s_invuln > 0) s_invuln--;
}

static void spawn_enemy(void) {
    int slot = -1;
    for (int i = 0; i < MAX_ENEMIES; ++i)
        if (!s_enemies[i].active) { slot = i; break; }
    if (slot < 0) return;

    uint32_t ang = prg32_random_number(0, 31);
    uint32_t radius = prg32_random_number(ENEMY_SPAWN_RADIUS_MIN, ENEMY_SPAWN_RADIUS_MAX);
    int ex = (int)(s_player.x_q4 / Q4) + (int)((cm_cos_table[ang] * (int)radius) >> 8);
    int ey = (int)(s_player.y_q4 / Q4) + (int)((cm_sin_table[ang] * (int)radius) >> 8);
    ex = duke_clamp(ex, CAR_HALF, WORLD_PX_W - CAR_HALF - 1);
    ey = duke_clamp(ey, CAR_HALF, WORLD_PX_H - CAR_HALF - 1);
    if (box_blocked(ex, ey)) return; /* try again next timer tick */

    s_enemies[slot].x_q4 = (int32_t)ex * Q4;
    s_enemies[slot].y_q4 = (int32_t)ey * Q4;
    s_enemies[slot].active = 1;
    s_enemies[slot].heading8 = 4;
    /* Twice as many scooters as police cars. */
    s_enemies[slot].kind = (prg32_random_number(0, 2) == 0) ? 1 : 0;
}

static void update_enemies(uint32_t pressed) {
    s_spawn_timer--;
    if (s_spawn_timer <= 0) {
        spawn_enemy();
        s_spawn_timer = (int)prg32_random_number(70, 150);
    }

    int px = (int)(s_player.x_q4 / Q4), py = (int)(s_player.y_q4 / Q4);
    int honk = (pressed & PRG32_BTN_A) != 0;
    if (honk) prg32_buzzer_tone(300, 120, 600);

    for (int i = 0; i < MAX_ENEMIES; ++i) {
        duke_enemy_t *e = &s_enemies[i];
        if (!e->active) continue;

        int ex = (int)(e->x_q4 / Q4), ey = (int)(e->y_q4 / Q4);
        int dxp = px - ex, dyp = py - ey;

        if (honk && e->kind == 0 && duke_abs(dxp) < 40 && duke_abs(dyp) < 40) {
            e->active = 0; /* scooter scared off by the horn */
            continue;
        }

        if (duke_abs(dxp) > ENEMY_DESPAWN_RADIUS || duke_abs(dyp) > ENEMY_DESPAWN_RADIUS) {
            e->active = 0;
            continue;
        }

        int step = (e->kind == 0) ? 3 : 2; /* scooters are quicker, police relentless */
        int nx = ex + (dxp > 0 ? step : (dxp < 0 ? -step : 0));
        int ny = ey + (dyp > 0 ? step : (dyp < 0 ? -step : 0));
        if (!box_blocked(nx, ey)) ex = nx;
        if (!box_blocked(ex, ny)) ey = ny;
        e->x_q4 = (int32_t)ex * Q4;
        e->y_q4 = (int32_t)ey * Q4;
        e->heading8 = (uint8_t)heading8_for_delta(dxp, dyp);

        if (s_invuln == 0 &&
            prg32_sprite_hitbox(px - CAR_HALF, py - CAR_HALF, CAR_HALF * 2, CAR_HALF * 2,
                                ex - CAR_HALF, ey - CAR_HALF, CAR_HALF * 2, CAR_HALF * 2)) {
            s_trouble++;
            s_invuln = INVULN_FRAMES;
            e->active = 0;
            prg32_buzzer_tone(150, 200, 700);
            if (s_trouble >= TROUBLE_MAX) {
                s_state = ST_LOSE;
                s_lose_reason = (e->kind == 0) ? REASON_SCOOTER : REASON_POLICE;
            }
        }
    }
}

static void update_items_and_party(void) {
    int px = (int)(s_player.x_q4 / Q4), py = (int)(s_player.y_q4 / Q4);
    for (int i = 0; i < CM_ITEM_COUNT; ++i) {
        if (s_collected[i]) continue;
        int ix = cm_item_points[i].x * CM_TILE_PX + 4;
        int iy = cm_item_points[i].y * CM_TILE_PX + 4;
        if (prg32_sprite_hitbox(px - CAR_HALF, py - CAR_HALF, CAR_HALF * 2, CAR_HALF * 2,
                                ix - 4, iy - 4, 8, 8)) {
            s_collected[i] = 1;
            s_collected_count++;
            prg32_buzzer_tone(1200, 60, 500);
        }
    }

    int ptx = px >> 3, pty = py >> 3;
    if (cm_tile_at(ptx, pty) == CM_T_PARTY && s_collected_count >= CM_ITEM_COUNT) {
        s_state = ST_WIN;
        s_win_time = (int)s_frame;
    }
}

static void update_camera(void) {
    int px = (int)(s_player.x_q4 / Q4), py = (int)(s_player.y_q4 / Q4);
    s_camera_x = duke_clamp(px - PRG32_GAME_W / 2, 0, WORLD_PX_W - PRG32_GAME_W);
    s_camera_y = duke_clamp(py - PRG32_GAME_H / 2, 0, WORLD_PX_H - PRG32_GAME_H);
    prg32_playfield_camera(s_camera_x, s_camera_y);
    stream_to(duke_floor_div8(s_camera_x), duke_floor_div8(s_camera_y));
}

/* ---- drawing ------------------------------------------------------------
 */
static void draw_bar(int x, int y, int w, int h, int value, int max_value,
                     uint16_t fg, uint16_t bg) {
    prg32_gfx_rect(x, y, w, h, bg);
    int fw = max_value > 0 ? (w * value) / max_value : 0;
    if (fw > 0) prg32_gfx_rect(x, y, fw, h, fg);
    prg32_gfx_rect(x, y, w, 1, PRG32_COLOR_BLACK);
    prg32_gfx_rect(x, y + h - 1, w, 1, PRG32_COLOR_BLACK);
}

#define DUKE_VEHICLE_FRAMES 8 /* N,NE,E,SE,S,SW,W,NW -- matches gen_assets.py */

static void draw_vehicle(int screen_x, int screen_y, const uint8_t *pixels,
                         const uint16_t *palette, uint16_t w, uint16_t h,
                         uint16_t palcount, uint8_t heading8) {
    prg32_indexed_sprite_t s;
    sprite_from(&s, pixels, palette, w, h, DUKE_VEHICLE_FRAMES, palcount, 4);
    prg32_sprite_draw_indexed(screen_x - w / 2, screen_y - h / 2, &s, heading8);
}

static void draw_world(void) {
    prg32_playfield_draw_dual();

    /* Party villa landmark: a genuinely 8bpp indexed sprite (8 colours),
     * unlike the 1bpp engine tiles used for the repeating street fabric. */
    int villa_wx = cm_party_point.x * CM_TILE_PX + 4 - DUKE_VILLA_WIDTH / 2;
    int villa_wy = cm_party_point.y * CM_TILE_PX + 4 - DUKE_VILLA_HEIGHT / 2;
    prg32_indexed_sprite_t villa;
    sprite_from(&villa, duke_villa_pixels, duke_villa_palette, DUKE_VILLA_WIDTH,
               DUKE_VILLA_HEIGHT, 1, DUKE_VILLA_PALETTE_COUNT, 8);
    prg32_sprite_draw_indexed(villa_wx - s_camera_x, villa_wy - s_camera_y, &villa, 0);

    for (int i = 0; i < cm_gas_point_count; ++i) {
        int sx = cm_gas_points[i].x * CM_TILE_PX + 4 - s_camera_x;
        int sy = cm_gas_points[i].y * CM_TILE_PX + 4 - s_camera_y;
        prg32_sprite_draw_8x8(sx - 4, sy - 4, duke_icon_gas, PRG32_COLOR_WHITE, PRG32_COLOR_RED);
    }

    for (int i = 0; i < CM_ITEM_COUNT; ++i) {
        if (s_collected[i]) continue;
        int sx = cm_item_points[i].x * CM_TILE_PX + 4 - s_camera_x;
        int sy = cm_item_points[i].y * CM_TILE_PX + 4 - s_camera_y;
        prg32_sprite_draw_8x8(sx - 4, sy - 4, duke_item_icons[i], duke_item_colors[i],
                              PRG32_COLOR_BLACK);
    }

    for (int i = 0; i < MAX_ENEMIES; ++i) {
        if (!s_enemies[i].active) continue;
        int sx = (int)(s_enemies[i].x_q4 / Q4) - s_camera_x;
        int sy = (int)(s_enemies[i].y_q4 / Q4) - s_camera_y;
        if (s_enemies[i].kind == 0) {
            draw_vehicle(sx, sy, duke_scooter_pixels, duke_scooter_palette,
                        DUKE_SCOOTER_WIDTH, DUKE_SCOOTER_HEIGHT,
                        DUKE_SCOOTER_PALETTE_COUNT, s_enemies[i].heading8);
        } else {
            draw_vehicle(sx, sy, duke_police_pixels, duke_police_palette,
                        DUKE_POLICE_WIDTH, DUKE_POLICE_HEIGHT,
                        DUKE_POLICE_PALETTE_COUNT, s_enemies[i].heading8);
        }
    }

    int psx = (int)(s_player.x_q4 / Q4) - s_camera_x;
    int psy = (int)(s_player.y_q4 / Q4) - s_camera_y;
    if (s_invuln == 0 || (s_frame & 2) == 0) {
        draw_vehicle(psx, psy, duke_car_pixels, duke_car_palette, DUKE_CAR_WIDTH,
                    DUKE_CAR_HEIGHT, DUKE_CAR_PALETTE_COUNT, s_player.heading >> 2);
    }
}

static void draw_hud(void) {
    draw_bar(6, 6, 80, 8, s_fuel, FUEL_MAX,
            s_fuel < FUEL_MAX / 5 ? PRG32_COLOR_RED : PRG32_COLOR_GREEN, 0x2104);
    prg32_gfx_text8(6, 16, "GAS", PRG32_COLOR_WHITE, PRG32_COLOR_BLACK);

    for (int i = 0; i < CM_ITEM_COUNT; ++i) {
        int x = 96 + i * 10;
        prg32_sprite_draw_8x8(x, 6, duke_item_icons[i],
                              s_collected[i] ? duke_item_colors[i] : 0x39c7,
                              PRG32_COLOR_BLACK);
    }
    char buf[6];
    prg32_gfx_text8(96, 16, "ITEMS ", PRG32_COLOR_WHITE, PRG32_COLOR_BLACK);
    duke_utoa((unsigned)s_collected_count, buf, sizeof(buf));
    prg32_gfx_text8(144, 16, buf, PRG32_COLOR_YELLOW, PRG32_COLOR_BLACK);
    prg32_gfx_text8(152, 16, "/8", PRG32_COLOR_WHITE, PRG32_COLOR_BLACK);

    prg32_gfx_text8(230, 6, "TROUBLE", PRG32_COLOR_WHITE, PRG32_COLOR_BLACK);
    for (int i = 0; i < TROUBLE_MAX; ++i) {
        prg32_gfx_rect(230 + i * 10, 16, 8, 8,
                      i < s_trouble ? PRG32_COLOR_RED : 0x39c7);
    }

    if (s_collected_count >= CM_ITEM_COUNT) {
        int px = (int)(s_player.x_q4 / Q4), py = (int)(s_player.y_q4 / Q4);
        int dxp = cm_party_point.x * CM_TILE_PX - px;
        int dyp = cm_party_point.y * CM_TILE_PX - py;
        const char *arrow = "*";
        if (duke_abs(dxp) > duke_abs(dyp)) arrow = dxp > 0 ? ">" : "<";
        else arrow = dyp > 0 ? "v" : "^";
        prg32_gfx_text8(150, 180, "TO THE PARTY", PRG32_COLOR_MAGENTA, PRG32_COLOR_BLACK);
        prg32_gfx_text8(292, 180, arrow, PRG32_COLOR_MAGENTA, PRG32_COLOR_BLACK);
    }
}

static void draw_centered_panel(const char *lines[], int count, uint16_t accent) {
    int h = 20 + count * 12;
    int y = (PRG32_GAME_H - h) / 2;
    prg32_gfx_rect(30, y, PRG32_GAME_W - 60, h, PRG32_COLOR_BLACK);
    prg32_gfx_rect(30, y, PRG32_GAME_W - 60, 2, accent);
    prg32_gfx_rect(30, y + h - 2, PRG32_GAME_W - 60, 2, accent);
    for (int i = 0; i < count; ++i) {
        int len = 0;
        while (lines[i][len]) len++;
        int x = (PRG32_GAME_W - len * 8) / 2;
        prg32_gfx_text8(x, y + 10 + i * 12, lines[i], PRG32_COLOR_WHITE, PRG32_COLOR_BLACK);
    }
}

static void draw_title(void) {
    prg32_gfx_clear(0x1a37);
    for (int i = 0; i < 24; ++i) {
        prg32_gfx_rect((int)(i * 137u % 320u), (int)(i * 71u % 60u), 2, 2, PRG32_COLOR_WHITE);
    }
    prg32_gfx_rect(0, 150, PRG32_GAME_W, 50, 0x2a5c);
    prg32_gfx_text8(56, 60, "DUKES OF DUCHESCA", PRG32_COLOR_YELLOW, 0x1a37);
    prg32_gfx_text8(104, 76, "- NAPOLI 97 -", PRG32_COLOR_WHITE, 0x1a37);
    prg32_gfx_text8(24, 104, "STEAL BACK THE NIGHT IN A", PRG32_COLOR_WHITE, 0x1a37);
    prg32_gfx_text8(24, 116, "PIMPED-UP FIAT 500 CLASSIC", PRG32_COLOR_WHITE, 0x1a37);
    prg32_gfx_text8(24, 132, "COLLECT ALL 8 PARTY ITEMS,", PRG32_COLOR_CYAN, 0x1a37);
    prg32_gfx_text8(24, 144, "DODGE SCOOTERS & POLICE,", PRG32_COLOR_CYAN, 0x1a37);
    prg32_gfx_text8(24, 156, "WATCH THE GAS, REACH THE PARTY", PRG32_COLOR_CYAN, 0x1a37);
    if ((s_frame >> 4) & 1) {
        prg32_gfx_text8(96, 180, "PRESS START", PRG32_COLOR_MAGENTA, 0x2a5c);
    }
}

/* ---- exported ABI: dukes_init / dukes_update / dukes_draw -------------- */
void dukes_init(void) {
    define_tiles();
    fill_backdrop_once();
    prg32_playfield_clear(1, CM_T_BUILDING);
    s_state = ST_TITLE;
    s_last_input = 0;
    s_frame = 0;
    reset_game();
}

void dukes_update(void) {
    uint32_t input = prg32_input_read();
    uint32_t pressed = input & ~s_last_input;
    s_last_input = input;
    s_frame++;

    switch (s_state) {
    case ST_TITLE:
        if (pressed & PRG32_BTN_START) {
            reset_game();
            s_state = ST_PLAYING;
        }
        break;
    case ST_PLAYING:
        if (pressed & PRG32_BTN_START) {
            s_state = ST_PAUSED;
            break;
        }
        update_player(input);
        update_enemies(pressed);
        update_items_and_party();
        update_camera();
        break;
    case ST_PAUSED:
        if (pressed & PRG32_BTN_START) s_state = ST_PLAYING;
        break;
    case ST_WIN:
    case ST_LOSE:
        if (pressed & PRG32_BTN_START) s_state = ST_TITLE;
        break;
    }
}

void dukes_draw(void) {
    if (s_state == ST_TITLE) {
        draw_title();
        return;
    }

    draw_world();
    draw_hud();

    if (s_state == ST_PAUSED) {
        const char *lines[] = {"PAUSED", "PRESS START TO RESUME"};
        draw_centered_panel(lines, 2, PRG32_COLOR_CYAN);
    } else if (s_state == ST_WIN) {
        const char *lines[] = {"PARTY TIME!", "YOU MADE IT TO THE VILLA",
                               "PRESS START"};
        draw_centered_panel(lines, 3, PRG32_COLOR_YELLOW);
    } else if (s_state == ST_LOSE) {
        const char *l1 = "GAME OVER";
        const char *l2 = "THEY STOLE THE CINQUECENTO!";
        if (s_lose_reason == REASON_POLICE) l2 = "BUSTED BY THE POLIZIA!";
        if (s_lose_reason == REASON_GAS) l2 = "RAN DRY IN THE VICOLI!";
        const char *lines[] = {l1, l2, "PRESS START"};
        draw_centered_panel(lines, 3, PRG32_COLOR_RED);
    }
}

#ifdef DUKE_HOST_TEST
/* Read-only accessors compiled in only for tests/host_harness.c. Never
 * defined in the real cartridge build (build.sh does not set
 * DUKE_HOST_TEST), so this block ships zero bytes to the cartridge. */
int duke_test_state(void) { return s_state; }
int duke_test_fuel(void) { return s_fuel; }
int duke_test_trouble(void) { return s_trouble; }
int duke_test_collected_count(void) { return s_collected_count; }
int duke_test_player_x(void) { return (int)(s_player.x_q4 / Q4); }
int duke_test_player_y(void) { return (int)(s_player.y_q4 / Q4); }
int duke_test_player_tile_solid(void) {
    int tx = duke_test_player_x() >> 3;
    int ty = duke_test_player_y() >> 3;
    return cm_tile_is_solid(cm_tile_at(tx, ty));
}
int duke_test_active_enemy_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i) n += s_enemies[i].active ? 1 : 0;
    return n;
}
#endif
