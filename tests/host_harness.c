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
int duke_test_speed(void);
int duke_test_halt(void);
int duke_test_searches(void);
int duke_test_chasing(void);
int duke_test_enemy_kind(int i);
void duke_test_place_enemy(int i, int kind, int x, int y);
void duke_test_teleport(int x, int y);
void duke_test_clear_enemies(void);
int duke_test_traffic(int i, int *x, int *y, int *kind, int *dir);
void duke_test_place_traffic(int i, int kind, int dir, int x, int y);

enum { ST_TITLE = 0, ST_PLAYING, ST_PAUSED, ST_WIN, ST_LOSE };
#define FUEL_MAX 3600
#define MAX_ENEMIES 6
#define SPEED_LIMIT 32
#define KIND_SCOOTER 0
#define KIND_POLICE 1
#define MAX_TRAFFIC 7
#define TRAFFIC_BUS 4
#define TRAFFIC_TRUCK 5

#define BTN_LEFT (1u << 0)
#define BTN_RIGHT (1u << 1)
#define BTN_UP (1u << 2)
#define BTN_DOWN (1u << 3)
#define BTN_A (1u << 4)
#define BTN_B (1u << 5)
#define BTN_START (1u << 6)

static const char *s_dump_dir;
static int s_teleported; /* a test put the car somewhere by hand this frame */
static int s_poi_pending, s_poi_shot;
static long s_frames, s_parity_frames;
static int s_max_enemies_seen, s_box_seen, s_cars_seen, s_buses_seen, s_trucks_seen, s_traffic_shot;

static int traffic_half(int kind) { return kind == TRAFFIC_BUS ? 11 : (kind == TRAFFIC_TRUCK ? 10 : 6); }
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
        for (int i = 0; i < MAX_TRAFFIC; ++i) {
            int tx, ty, kind, dir;
            if (!duke_test_traffic(i, &tx, &ty, &kind, &dir)) continue;
            /* Traffic keeps to the streets and never runs the Fiat over. */
            uint8_t under = cm_tile_at(tx >> 3, ty >> 3);
            assert(under == CM_T_ROAD || under == CM_T_ROAD_LINE || under == CM_T_PROMENADE);
            int length = traffic_half(kind);
            int hx = (dir & 1) ? length : 4, hy = (dir & 1) ? 4 : length;
            if (st == ST_PLAYING && !s_teleported) {
                assert(abs(px - tx) >= 4 + hx || abs(py - ty) >= 4 + hy);
                /* Nor a scooter or a patrol car, nor one another. */
                for (int e = 0; e < MAX_ENEMIES; ++e) {
                    int ex, ey;
                    if (duke_test_enemy(e, &ex, &ey))
                        assert(abs(ex - tx) >= 4 + hx || abs(ey - ty) >= 4 + hy);
                }
                for (int j = i + 1; j < MAX_TRAFFIC; ++j) {
                    int ox, oy, okind, odir;
                    if (!duke_test_traffic(j, &ox, &oy, &okind, &odir)) continue;
                    int olen = traffic_half(okind);
                    assert(abs(ox - tx) >= hx + ((odir & 1) ? olen : 4) ||
                           abs(oy - ty) >= hy + ((odir & 1) ? 4 : olen));
                }
            }
            if (kind == TRAFFIC_BUS) s_buses_seen++;
            else if (kind == TRAFFIC_TRUCK) s_trucks_seen++;
            else s_cars_seen++;
            if (kind == TRAFFIC_BUS && !s_traffic_shot && abs(px - tx) < 50 && abs(py - ty) < 30 &&
                duke_test_dusk() < 8) {
                dump("14-traffic"); s_traffic_shot = 1;
            }
        }
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

/* A careful driver lifts off before a checkpoint and when a patrol is near. */
static int should_ease_off(void) {
    int px = duke_test_player_x(), py = duke_test_player_y();
    if (duke_test_speed() <= SPEED_LIMIT - 8) return 0;
    for (int i = 0; i < CM_CHECKPOINT_COUNT; ++i) {
        int cx = cm_checkpoints[i].x * 8 + cm_checkpoints[i].w * 4;
        int cy = cm_checkpoints[i].y * 8 + cm_checkpoints[i].h * 4;
        if (abs(px - cx) < 48 && abs(py - cy) < 48) return 1;
    }
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        int ex, ey;
        if (duke_test_enemy(i, &ex, &ey) && duke_test_enemy_kind(i) == KIND_POLICE &&
            abs(px - ex) < 64 && abs(py - ey) < 64)
            return 1;
    }
    return 0;
}

static uint32_t autopilot(void) {
    choose_goal();
    return should_ease_off() ? 0 : follow_field();
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
            frame(should_ease_off() ? 0 : follow_field());
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
    assert(duke_test_poi() == CM_AT_ROCK_GARDEN);
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
    int drop_y = duke_test_player_y();
    for (int i = 0; i < 59; ++i) frame(BTN_UP);
    assert(duke_test_lit() == 1);                 /* 60 ticks: still burning */
    dump("09-rauto-lit");
    frame(BTN_UP);
    assert(duke_test_lit() == 0);                 /* 61 ticks = 2 s: bang */
    assert(drop_y - duke_test_player_y() > 30);
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

    /* Traffic is solid and patient. A bus stands across the street ahead: the
     * Fiat, held straight at it, is steered round it and gets past; a car
     * coming the other way stops, or changes lane, rather than drive into the
     * standing Fiat. */
    int tx0, ty0, tk, td;
    s_teleported = 1;
    duke_test_teleport(110 * 8 + 4, 49 * 8 + 4);
    duke_test_place_traffic(0, TRAFFIC_BUS, 1, 116 * 8 + 4, 49 * 8 + 4); /* standing in the middle lane */
    for (int i = 0; i < 6; ++i) {
        frame(BTN_RIGHT);
        duke_test_place_traffic(0, TRAFFIC_BUS, 1, 116 * 8 + 4, 49 * 8 + 4); /* hold it there */
    }
    for (int i = 0; i < 60 && duke_test_player_x() < 122 * 8; ++i) {
        frame(BTN_RIGHT);
        duke_test_place_traffic(0, TRAFFIC_BUS, 1, 116 * 8 + 4, 49 * 8 + 4);
        if (i == 12) dump("15-overtaking");
    }
    assert(duke_test_player_x() >= 122 * 8);              /* round it, not through it */
    assert(duke_test_player_y() != 49 * 8 + 4);
    duke_test_clear_enemies();
    duke_test_teleport(110 * 8 + 4, 49 * 8 + 4);
    duke_test_place_traffic(0, 1, 3, 116 * 8 + 4, 49 * 8 + 4);           /* a car heading west, at the Fiat */
    int closest = 1000;
    for (int i = 0; i < 100; ++i) {
        frame(0);
        assert(duke_test_traffic(0, &tx0, &ty0, &tk, &td));
        int gap_x = abs(tx0 - duke_test_player_x()), gap_y = abs(ty0 - duke_test_player_y());
        /* Never inside the Fiat's box: clear along its length or across it. */
        assert(gap_x >= 4 + ((td & 1) ? 6 : 4) || gap_y >= 4 + ((td & 1) ? 4 : 6));
        if (gap_x + gap_y < closest) closest = gap_x + gap_y;
    }
    assert(closest <= 16); /* and it did come right up to it */
    assert(duke_test_trouble() == 1);
    duke_test_clear_enemies();

    /* The trash truck crawls, and stops for three seconds at a time. */
    duke_test_teleport(104 * 8 + 4, 46 * 8 + 4); /* watching from the side street */
    duke_test_place_traffic(0, TRAFFIC_TRUCK, 1, 97 * 8 + 4, 49 * 8 + 4);
    int moved = 0, still = 0, longest = 0, last_x = 0, last_y = 0, start_x;
    assert(duke_test_traffic(0, &start_x, &last_y, &tk, &td));
    last_x = start_x;
    for (int i = 0; i < 420; ++i) {
        frame(0);
        assert(duke_test_traffic(0, &tx0, &ty0, &tk, &td) && tk == TRAFFIC_TRUCK);
        if (tx0 == last_x && ty0 == last_y) { if (++still > longest) longest = still; }
        else { still = 0; moved += abs(tx0 - last_x) + abs(ty0 - last_y); }
        last_x = tx0; last_y = ty0;
        if (still == 45) dump("16-trash-truck");
    }
    assert(longest >= 85 && longest <= 95);  /* three seconds at a stop */
    assert(moved > 60 && moved < 420 * 5 / 8 + 2); /* 0.625 pixels a tick, when it moves at all */
    /* A scooter that finds it across its lane does not drive through it:
     * it turns back and takes another way. */
    duke_test_clear_enemies();
    duke_test_place_traffic(0, TRAFFIC_BUS, 1, 116 * 8 + 4, 49 * 8 + 4);
    duke_test_place_enemy(0, KIND_SCOOTER, 124 * 8 + 4, 49 * 8 + 4);
    duke_test_teleport(104 * 8 + 4, 49 * 8 + 4);
    int got_round = 0;
    for (int i = 0; i < 90 && !got_round; ++i) {
        int sx, sy;
        duke_test_place_traffic(0, TRAFFIC_BUS, 1, 116 * 8 + 4, 49 * 8 + 4); /* the bus stands still */
        frame(0);
        assert(duke_test_enemy(0, &sx, &sy));
        assert(abs(sx - (116 * 8 + 4)) >= 4 + 11 || abs(sy - (49 * 8 + 4)) >= 8);
        got_round = sx < 116 * 8 + 4 - 20;
    }
    assert(got_round);                /* past the bus, by another lane */
    assert(duke_test_trouble() == 1);
    duke_test_clear_enemies();
    s_teleported = 0;

    /* The police. A scooter with a patrol car beside it runs from the patrol,
     * not at the Fiat. Decumano row 49 of the centro storico is a long
     * straight street, clear of checkpoints. */
    int ex, ey, px0, py0;
    duke_test_teleport(110 * 8 + 4, 49 * 8 + 4);
    duke_test_place_enemy(0, KIND_POLICE, 119 * 8 + 4, 49 * 8 + 4);
    duke_test_place_enemy(1, KIND_SCOOTER, 122 * 8 + 4, 49 * 8 + 4);
    for (int i = 0; i < 25; ++i) frame(0);
    assert(duke_test_enemy(0, &px0, &py0) && duke_test_enemy(1, &ex, &ey));
    assert(abs(ex - px0) + abs(ey - py0) > 40);   /* it began 24 pixels from the patrol */
    assert(abs(ex - duke_test_player_x()) > 60);
    assert(duke_test_trouble() == 1 && duke_test_searches() == 0);
    /* A patrol does not mind a Fiat rolling by slowly, bumper to bumper... */
    duke_test_clear_enemies();
    duke_test_place_enemy(0, KIND_POLICE, duke_test_player_x() + 30, duke_test_player_y());
    for (int i = 0; i < 40; ++i) frame(duke_test_speed() < 16 ? BTN_RIGHT : 0);
    assert(duke_test_chasing() == 0 && duke_test_searches() == 0 && duke_test_trouble() == 1);
    /* ...but one flat out under its nose is chased, and searched when caught. */
    duke_test_clear_enemies();
    duke_test_teleport(110 * 8 + 4, 49 * 8 + 4);
    duke_test_place_enemy(0, KIND_POLICE, 117 * 8 + 4, 49 * 8 + 4);
    duke_test_set_rauti(20);
    int chased = 0;
    for (int i = 0; i < 45 && !chased; ++i) {
        frame(BTN_RIGHT);
        /* A patrol this close may catch the Fiat in the tick it starts after it. */
        chased = duke_test_chasing() || duke_test_searches();
    }
    assert(chased == 1);
    dump("11-chase");
    for (int i = 0; i < 300 && duke_test_searches() == 0; ++i) frame(0);
    assert(duke_test_searches() == 1 && duke_test_halt() > 0 && duke_test_chasing() == 0);
    assert(duke_test_rauti() == 0 || duke_test_rauti() == 20);
    int held_x = duke_test_player_x();
    for (int i = 0; i < 30; ++i) frame(BTN_RIGHT); /* halted: the car does not answer */
    assert(duke_test_player_x() == held_x && duke_test_halt() > 0);
    dump("12-search");
    for (int i = 0; i < 60; ++i) frame(0);
    assert(duke_test_halt() == 0);
    duke_test_clear_enemies();

    /* Checkpoints. Through the one on the decumano towards Chiaia flat out:
     * flagged down. The police then leave the Fiat alone for a while. */
    for (int i = 0; i < 260; ++i) frame(0);
    duke_test_teleport(111 * 8 + 4, 81 * 8 + 4);
    for (int i = 0; i < 40 && duke_test_searches() == 1; ++i) frame(BTN_RIGHT);
    assert(duke_test_searches() == 2 && duke_test_halt() > 0);
    assert(cm_checkpoint_at(duke_test_player_x() >> 3, duke_test_player_y() >> 3) == 0);
    for (int i = 0; i < 400; ++i) frame(0);
    /* Rolled through below the limit: waved on. */
    int score = duke_test_score();
    duke_test_teleport(111 * 8 + 4, 81 * 8 + 4);
    for (int i = 0; i < 200 && (duke_test_player_x() >> 3) < 124; ++i)
        frame(duke_test_speed() < SPEED_LIMIT - 10 ? BTN_RIGHT : 0);
    assert((duke_test_player_x() >> 3) >= 124);
    assert(duke_test_searches() == 2 && duke_test_score() == score + 25);
    dump("13-checkpoint");
    duke_test_set_rauti(0);
    for (int i = 0; i < 260; ++i) frame(0);

    long before = s_frames;
    int result = drive(40000, 0, 1);
    printf("autopilot, empty streets: state=%d items=%d/8 fuel=%d score=%d in %ld frames (%.0f s)\n",
           result, duke_test_collected_count(), duke_test_fuel(), duke_test_score(),
           s_frames - before, (double)(s_frames - before) * 0.033);
    assert(result == ST_WIN);
    assert(duke_test_collected_count() == CM_ITEM_COUNT);
    assert(duke_test_trouble() == 1);
    assert(duke_test_searches() == 2); /* the careful driver was not stopped again */
    assert(g_test_track == 2);
    assert(g_test_last_score == (uint32_t)duke_test_score());
    for (int i = 0; i < 60; ++i) frame(0);
    dump("07-win");

    start_game();
    s_poi_seen = 0; /* photograph every place again, by daylight */
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
    int wins = 0, losses = 0, unfinished = 0, searches = 0;
    for (int night = 0; night < 12; ++night) {
        start_game();
        result = drive(40000, 1, 1);
        searches += duke_test_searches();
        if (result == ST_WIN) wins++;
        else if (result == ST_PLAYING) unfinished++;
        else {
            losses++;
            if (losses == 1) { for (int i = 0; i < 40; ++i) frame(0); dump("08-lose"); }
        }
    }
    printf("traffic: cars on %d frames, buses on %d, the trash truck on %d\n", s_cars_seen, s_buses_seen,
           s_trucks_seen);
    assert(s_cars_seen > 1000 && s_buses_seen > 1000 && s_trucks_seen > 1000);
    printf("rauti: a box was picked up in traffic: %s\n", s_box_seen ? "yes" : "no");
    assert(s_box_seen);
    printf("autopilot, in traffic: %d wins, %d losses, %d unfinished, %d searches, up to %d cars around\n",
           wins, losses, unfinished, searches, s_max_enemies_seen);
    assert(wins + losses > 0);
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
