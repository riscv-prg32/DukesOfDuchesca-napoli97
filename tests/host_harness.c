/* Dynamic host harness: runs the real dukes_init/update/draw loop (game.c
 * compiled with DUKE_HOST_TEST, linked against tests/prg32_stub.c).
 *
 *  1. An autopilot drives the whole game with the chasers off and must win:
 *     every item collected, refuelling on the way, party reached.
 *  2. The autopilot plays again with scooters and police on the streets.
 *  3. 60,000 fuzzed frames.
 *
 * Every frame asserts the runtime invariants and that the two display back
 * ends (QEMU's RGB surface and the ESP32-C6's indexed framebuffer) show the
 * same picture. Pass a directory as argv[1] to get PPM frames of each screen.
 */
#include "../citymap.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define GAME_W 320
#define GAME_H 200

extern uint32_t g_test_input, g_test_ms, g_test_last_score;
extern int g_test_track, g_test_tempo, g_test_notes;
extern uint16_t g_qemu_fb[GAME_H][GAME_W];
extern uint8_t g_c6_fb[GAME_H][GAME_W];
extern uint16_t g_palette[256];
int stub_dump_ppm(const char *path);

void dukes_init(void);
void dukes_update(void);
void dukes_draw(void);
int duke_test_state(void);
int duke_test_fuel(void);
int duke_test_trouble(void);
int duke_test_collected_count(void);
int duke_test_collected(int i);
int duke_test_player_x(void);
int duke_test_player_y(void);
int duke_test_heading(void);
int duke_test_score(void);
int duke_test_dusk(void);
int duke_test_enemy_count(void);
int duke_test_enemy(int i, int *x, int *y);
void duke_test_enemies(int on);
void duke_test_set_dusk(int dusk);
int duke_test_rauti(void);
void duke_test_set_rauti(int n);
int duke_test_lit(void);
int duke_test_poi(void);
void duke_test_refuel(void);

enum { ST_TITLE = 0, ST_PLAYING, ST_PAUSED, ST_WIN, ST_LOSE };
#define FUEL_MAX 3600
#define MAX_ENEMIES 4

#define BTN_LEFT (1u << 0)
#define BTN_RIGHT (1u << 1)
#define BTN_UP (1u << 2)
#define BTN_DOWN (1u << 3)
#define BTN_A (1u << 4)
#define BTN_B (1u << 5)
#define BTN_START (1u << 6)

static const char *s_dump_dir;
static int s_poi_pending, s_poi_shot;
static long s_frames, s_parity_frames;
static int s_max_enemies_seen, s_box_seen;
static unsigned s_poi_seen; /* a bit per point of interest the car has been at */

static void dump(const char *name) {
    char path[512];
    if (!s_dump_dir) return;
    snprintf(path, sizeof(path), "%s/%s.ppm", s_dump_dir, name);
    assert(stub_dump_ppm(path) == 0);
}

/* One firmware frame: 33 ms pass, update, draw, then check everything. */
static void frame(uint32_t input) {
    g_test_input = input;
    g_test_ms += 33;
    dukes_update();
    dukes_draw();
    s_frames++;

    int st = duke_test_state();
    assert(st >= ST_TITLE && st <= ST_LOSE);
    assert(duke_test_fuel() >= 0 && duke_test_fuel() <= FUEL_MAX);
    assert(duke_test_trouble() >= 0 && duke_test_trouble() <= 3);
    assert(duke_test_collected_count() >= 0 && duke_test_collected_count() <= CM_ITEM_COUNT);
    assert(duke_test_score() >= 0 && duke_test_score() < 1000000);
    assert(duke_test_dusk() >= 0 && duke_test_dusk() <= 20);
    assert(duke_test_rauti() >= 0 && duke_test_rauti() <= CM_RAUTI_COUNT * 20);
    assert(duke_test_lit() >= 0 && duke_test_lit() <= 4);

    if (st != ST_TITLE) {
        int px = duke_test_player_x(), py = duke_test_player_y();
        if (cm_box_blocked(px, py)) {
            fprintf(stderr, "frame %ld: the car at (%d,%d) overlaps solid ground\n", s_frames, px, py);
            assert(0);
        }
        int n = 0;
        for (int i = 0; i < MAX_ENEMIES; ++i) {
            int ex, ey;
            if (!duke_test_enemy(i, &ex, &ey)) continue;
            n++;
            if (cm_tile_is_solid(cm_tile_at(ex >> 3, ey >> 3))) {
                fprintf(stderr, "frame %ld: enemy %d at (%d,%d) is inside a building\n", s_frames, i, ex, ey);
                assert(0);
            }
        }
        if (n > s_max_enemies_seen) s_max_enemies_seen = n;
        if (duke_test_rauti() > 0) s_box_seen = 1;
        if (duke_test_poi() >= 0) {
            if (!(s_poi_seen & (1u << duke_test_poi()))) {
                s_poi_pending = 12;
                s_poi_shot = duke_test_poi();
            }
            s_poi_seen |= 1u << duke_test_poi();
        }
    }

    /* A picture of each point of interest, a moment after arriving. */
    if (s_poi_pending && --s_poi_pending == 0) {
        char name[40];
        snprintf(name, sizeof(name), "poi-%02d", s_poi_shot);
        dump(name);
    }

    /* Both back ends must show the same picture, pixel for pixel. */
    int differing = 0;
    for (int y = 0; y < GAME_H; ++y)
        for (int x = 0; x < GAME_W; ++x)
            if (g_qemu_fb[y][x] != g_palette[g_c6_fb[y][x]]) differing++;
    if (differing) {
        fprintf(stderr, "frame %ld (state %d): %d pixels differ between QEMU and the ESP32-C6\n",
                s_frames, st, differing);
        dump("parity-failure");
        assert(0);
    }
    s_parity_frames++;
}

/* ---- autopilot: follows a breadth-first distance field over open tiles -- */
static int s_dist[CM_WORLD_ROWS][CM_WORLD_COLS];
static int s_queue[CM_WORLD_ROWS * CM_WORLD_COLS];

static void field_from(int tx, int ty) {
    static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    int head = 0, tail = 0;
    for (int y = 0; y < CM_WORLD_ROWS; ++y)
        for (int x = 0; x < CM_WORLD_COLS; ++x) s_dist[y][x] = -1;
    s_dist[ty][tx] = 0;
    s_queue[tail++] = ty * CM_WORLD_COLS + tx;
    while (head < tail) {
        int cur = s_queue[head++], x = cur % CM_WORLD_COLS, y = cur / CM_WORLD_COLS;
        for (int i = 0; i < 4; ++i) {
            int nx = x + dx[i], ny = y + dy[i];
            if (nx < 0 || ny < 0 || nx >= CM_WORLD_COLS || ny >= CM_WORLD_ROWS) continue;
            if (s_dist[ny][nx] >= 0 || cm_tile_is_solid(cm_tile_at(nx, ny))) continue;
            s_dist[ny][nx] = s_dist[y][x] + 1;
            s_queue[tail++] = ny * CM_WORLD_COLS + nx;
        }
    }
}

static int s_goal_x = -1, s_goal_y = -1, s_refuelling;

static void choose_goal(void) {
    int px = duke_test_player_x() >> 3, py = duke_test_player_y() >> 3;
    cm_point_t goal = cm_party_point;
    field_from(px, py);
    if (duke_test_fuel() < FUEL_MAX * 2 / 5) s_refuelling = 1;
    if (duke_test_fuel() > FUEL_MAX * 19 / 20) s_refuelling = 0;
    int best = -1;
    if (s_refuelling) {
        for (int i = 0; i < CM_GAS_COUNT; ++i) {
            int d = s_dist[cm_gas_points[i].y][cm_gas_points[i].x];
            if (best < 0 || d < best) { best = d; goal = cm_gas_points[i]; }
        }
    } else {
        for (int i = 0; i < CM_ITEM_COUNT; ++i) {
            if (duke_test_collected(i)) continue;
            int d = s_dist[cm_item_points[i].y][cm_item_points[i].x];
            if (best < 0 || d < best) { best = d; goal = cm_item_points[i]; }
        }
    }
    if (goal.x != s_goal_x || goal.y != s_goal_y) {
        s_goal_x = goal.x; s_goal_y = goal.y;
    }
    field_from(s_goal_x, s_goal_y);
}

/* The D-pad direction that follows the current distance field downhill. */
static uint32_t follow_field(void) {
    static const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
    int tx = duke_test_player_x() >> 3, ty = duke_test_player_y() >> 3;
    if (s_dist[ty][tx] == 0) return 0; /* on the spot: wait (for the pump, or the party) */

    /* Point the D-pad at the next tile on the shortest path, as a player
     * would; the car's own steering does the rest. */
    static const uint32_t pad[4] = {BTN_UP, BTN_RIGHT, BTN_DOWN, BTN_LEFT};
    for (int d = 0; d < 4; ++d) {
        int nx = tx + dx[d], ny = ty + dy[d];
        if (nx < 0 || ny < 0 || nx >= CM_WORLD_COLS || ny >= CM_WORLD_ROWS) continue;
        if (s_dist[ny][nx] >= 0 && s_dist[ny][nx] < s_dist[ty][tx]) return pad[d];
    }
    assert(0 && "no way forward");
    return 0;
}

static uint32_t autopilot(void) {
    choose_goal();
    return follow_field();
}

static void start_game(void) {
    while (duke_test_state() != ST_TITLE) { frame(BTN_START); frame(0); }
    frame(BTN_START);
    frame(0);
    assert(duke_test_state() == ST_PLAYING);
    assert(g_test_track == 1);
}

/* Returns the final state after at most `limit` frames. */
static int drive(int limit, int honk, int dumps) {
    static int shot_drive, shot_night, shot_gas;
    s_goal_x = s_goal_y = -1;
    s_refuelling = 0;
    for (int i = 0; i < limit && duke_test_state() == ST_PLAYING; ++i) {
        uint32_t input = autopilot();
        if (honk && (i % 46) == 0) input |= BTN_A;
        if (honk && (i % 70) == 35) input |= BTN_B; /* and a rauto behind, now and then */
        frame(input);
        if (!dumps) continue;
        if (!shot_drive && duke_test_collected_count() == 2 && duke_test_enemy_count() >= 2) {
            dump("03-drive"); shot_drive = 1;
        }
        if (!shot_gas && s_refuelling && duke_test_fuel() > FUEL_MAX / 2) { dump("04-gas"); shot_gas = 1; }
        /* The second half of the empty-streets drive is at full night, so the
         * night palette goes through the same per-frame checks as the day one. */
        if (!honk && duke_test_collected_count() == 4) duke_test_set_dusk(20);
        if (!shot_night && duke_test_dusk() == 20 && duke_test_collected_count() == 5) {
            dump("05-night"); shot_night = 1;
        }
    }
    return duke_test_state();
}

/* The grand tour: drive up to every point of interest, in the order of the
 * list, so each monument is drawn (and checked on both displays) from all
 * the angles of an approach, and its name comes up. */
static void grand_tour(void) {
    for (int poi = 0; poi < CM_POI_COUNT; ++poi) {
        int best = -1, gx = 0, gy = 0;
        field_from(duke_test_player_x() >> 3, duke_test_player_y() >> 3);
        for (int ty = 0; ty < CM_WORLD_ROWS; ++ty)
            for (int tx = 0; tx < CM_WORLD_COLS; ++tx)
                if (s_dist[ty][tx] >= 0 && cm_poi_near(tx, ty) == poi && (best < 0 || s_dist[ty][tx] < best)) {
                    best = s_dist[ty][tx]; gx = tx; gy = ty;
                }
        assert(best >= 0);
        field_from(gx, gy);
        int frames = 0;
        while (duke_test_poi() != poi || s_dist[duke_test_player_y() >> 3][duke_test_player_x() >> 3] != 0) {
            duke_test_refuel();
            duke_test_set_dusk(3); /* the tour is a day trip: daylight pictures */
            frame(follow_field());
            assert(duke_test_state() == ST_PLAYING);
            assert(++frames < 4000);
        }
        for (int i = 0; i < 14; ++i) { duke_test_set_dusk(3); frame(0); }
    }
}

int main(int argc, char **argv) {
    s_dump_dir = argc > 1 ? argv[1] : 0;

    dukes_init();
    assert(duke_test_state() == ST_TITLE);
    assert(g_test_track == 0);
    for (int i = 0; i < 90; ++i) frame(0);
    dump("01-title");

    /* 1. The whole game with the streets empty: it must be winnable. */
    duke_test_enemies(0);
    start_game();
    assert(duke_test_player_x() == cm_start_point.x * CM_TILE_PX + 4);
    for (int i = 0; i < 20; ++i) frame(0);
    assert(duke_test_poi() == CM_AT_PLEBISCITO);
    dump("02-start");
    frame(BTN_START);
    assert(duke_test_state() == ST_PAUSED);
    frame(0);
    dump("06-paused");
    frame(BTN_START);
    frame(0);
    assert(duke_test_state() == ST_PLAYING);

    /* Rauti. Without a box, B does nothing. */
    frame(BTN_B);
    frame(0);
    assert(duke_test_lit() == 0 && duke_test_rauti() == 0);
    /* One dropped and driven away from: it goes off two seconds later and
     * the Fiat, well clear by then, is not touched. */
    duke_test_set_rauti(20);
    frame(BTN_B);
    assert(duke_test_lit() == 1 && duke_test_rauti() == 19);
    int drop_x = duke_test_player_x();
    for (int i = 0; i < 59; ++i) frame(BTN_LEFT);
    assert(duke_test_lit() == 1);                 /* 60 ticks: still burning */
    dump("09-rauto-lit");
    frame(BTN_LEFT);
    assert(duke_test_lit() == 0);                 /* 61 ticks = 2 s: bang */
    assert(drop_x - duke_test_player_x() > 30);
    assert(duke_test_trouble() == 0);
    frame(0); frame(0);
    dump("10-rauto-bang");
    /* One dropped and sat on: the Fiat is damaged by its own rauto. */
    for (int i = 0; i < 40; ++i) frame(0);
    frame(BTN_B);
    assert(duke_test_rauti() == 18);
    for (int i = 0; i < 60; ++i) frame(0);
    assert(duke_test_lit() == 0 && duke_test_trouble() == 1);
    for (int i = 0; i < 70; ++i) frame(0);        /* let the invulnerability run out */
    duke_test_set_rauti(0);

    long before = s_frames;
    int result = drive(40000, 0, 1);
    printf("autopilot, empty streets: state=%d items=%d/8 fuel=%d score=%d in %ld frames (%.0f s)\n",
           result, duke_test_collected_count(), duke_test_fuel(), duke_test_score(),
           s_frames - before, (double)(s_frames - before) * 0.033);
    assert(result == ST_WIN);
    assert(duke_test_collected_count() == CM_ITEM_COUNT);
    assert(duke_test_trouble() == 1);
    assert(g_test_track == 2);
    assert(g_test_last_score == (uint32_t)duke_test_score());
    for (int i = 0; i < 60; ++i) frame(0);
    dump("07-win");

    start_game();
    long tour_start = s_frames;
    grand_tour();
    int places = 0;
    for (int i = 0; i < CM_POI_COUNT; ++i) places += (s_poi_seen >> i) & 1u;
    printf("grand tour: %d of %d points of interest visited in %ld frames\n", places, CM_POI_COUNT,
           s_frames - tour_start);
    assert(places == CM_POI_COUNT);

    /* 2. The same drive through traffic. Winning is not guaranteed; the
     * invariants are. Several nights, so both endings are exercised. */
    duke_test_enemies(1);
    int wins = 0, losses = 0;
    for (int night = 0; night < 12; ++night) {
        start_game();
        result = drive(40000, 1, 1);
        assert(result == ST_WIN || result == ST_LOSE);
        if (result == ST_WIN) wins++;
        else {
            losses++;
            if (losses == 1) { for (int i = 0; i < 40; ++i) frame(0); dump("08-lose"); }
        }
    }
    printf("rauti: a box was picked up in traffic: %s\n", s_box_seen ? "yes" : "no");
    assert(s_box_seen);
    printf("autopilot, in traffic: %d wins, %d losses, up to %d chasers at once, %d effect notes\n",
           wins, losses, s_max_enemies_seen, g_test_notes);
    assert(s_max_enemies_seen >= 2);
    assert(g_test_notes > 0);

    /* 3. Fuzzing, including long gaps between frames (a slow board). */
    uint32_t lcg = 2463534242u;
    for (int i = 0; i < 60000; ++i) {
        lcg ^= lcg << 13; lcg ^= lcg >> 17; lcg ^= lcg << 5;
        uint32_t input = 0;
        if (lcg & 1u) input |= BTN_UP;
        if ((lcg >> 1) & 1u) input |= BTN_DOWN;
        if ((lcg >> 2) & 1u) input |= BTN_LEFT;
        if ((lcg >> 3) & 1u) input |= BTN_RIGHT;
        if (((lcg >> 4) & 0x1fu) == 0) input |= BTN_A;
        if (((lcg >> 9) & 0xffu) == 0) input |= BTN_START;
        if (((lcg >> 17) & 0x3fu) == 0) g_test_ms += (lcg >> 23) & 0xffu;
        frame(input);
    }
    printf("fuzz: %ld frames, both display back ends identical on %ld of them\n", s_frames,
           s_parity_frames);
    printf("ALL HOST HARNESS CHECKS PASSED\n");
    return 0;
}
