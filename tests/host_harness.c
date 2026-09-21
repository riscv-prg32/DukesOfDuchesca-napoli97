/* Dynamic host harness: runs the real dukes_init/update/draw loop (compiled
 * from game.c with DUKE_HOST_TEST, linked against tests/prg32_stub.c) under
 * scripted and fuzzed input, asserting runtime invariants every frame. This
 * exercises the tile-streaming/camera math and collision code for real,
 * which a pure syntax check cannot. */
#include "../citymap.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define TROUBLE_MAX_MIRROR 3
#define FUEL_MAX_MIRROR 3000

extern uint32_t g_test_input;
void dukes_init(void);
void dukes_update(void);
void dukes_draw(void);
int duke_test_state(void);
int duke_test_fuel(void);
int duke_test_trouble(void);
int duke_test_collected_count(void);
int duke_test_player_x(void);
int duke_test_player_y(void);
int duke_test_player_tile_solid(void);
int duke_test_active_enemy_count(void);

#define BTN_LEFT (1u << 0)
#define BTN_RIGHT (1u << 1)
#define BTN_UP (1u << 2)
#define BTN_DOWN (1u << 3)
#define BTN_A (1u << 4)
#define BTN_B (1u << 5)
#define BTN_START (1u << 6)

static uint32_t s_lcg = 2463534242u;
static uint32_t next_rand(void) {
    s_lcg ^= s_lcg << 13;
    s_lcg ^= s_lcg >> 17;
    s_lcg ^= s_lcg << 5;
    return s_lcg;
}

static void check_invariants(int frame) {
    int st = duke_test_state();
    assert(st >= 0 && st <= 4);
    int fuel = duke_test_fuel();
    assert(fuel >= 0 && fuel <= FUEL_MAX_MIRROR);
    int trouble = duke_test_trouble();
    assert(trouble >= 0 && trouble <= TROUBLE_MAX_MIRROR);
    int collected = duke_test_collected_count();
    assert(collected >= 0 && collected <= CM_ITEM_COUNT);
    int enemies = duke_test_active_enemy_count();
    assert(enemies >= 0 && enemies <= 5);

    if (st == 1 /* ST_PLAYING */) {
        if (duke_test_player_tile_solid()) {
            fprintf(stderr,
                    "INVARIANT VIOLATION at frame %d: player at (%d,%d) is "
                    "inside a solid tile!\n",
                    frame, duke_test_player_x(), duke_test_player_y());
            assert(0);
        }
        int px = duke_test_player_x(), py = duke_test_player_y();
        assert(px >= 0 && px < CM_WORLD_COLS * CM_TILE_PX);
        assert(py >= 0 && py < CM_WORLD_ROWS * CM_TILE_PX);
    }
}

static void run_fuzz(int frames) {
    for (int i = 0; i < frames; ++i) {
        uint32_t r = next_rand();
        uint32_t input = 0;
        if (r & 1) input |= BTN_UP;
        if ((r >> 1) & 1) input |= BTN_DOWN;
        if ((r >> 2) & 1) input |= BTN_LEFT;
        if ((r >> 3) & 1) input |= BTN_RIGHT;
        if (((r >> 4) & 0x1f) == 0) input |= BTN_A;      /* occasional honk */
        if (((r >> 9) & 0xff) == 0) input |= BTN_START;  /* rare pause/restart */
        g_test_input = input;
        dukes_update();
        dukes_draw();
        check_invariants(i);
    }
}

/* Only valid to call from TITLE/WIN/LOSE: resets and enters PLAYING. */
static void press_start(void) {
    g_test_input = BTN_START;
    dukes_update();
    g_test_input = 0;
    dukes_update();
}

/* Gets into PLAYING from wherever we are: resets from TITLE/WIN/LOSE, or
 * simply resumes (keeping position) from PAUSED. Unlike press_start(), safe
 * to call regardless of current state. */
static void ensure_playing(void) {
    if (duke_test_state() != 1 /* ST_PLAYING */) press_start();
}

int main(void) {
    dukes_init();
    assert(duke_test_state() == 0 /* ST_TITLE */);

    press_start();
    assert(duke_test_state() == 1 /* ST_PLAYING */);
    assert(duke_test_player_x() == cm_start_point.x * CM_TILE_PX + 4);
    assert(duke_test_player_y() == cm_start_point.y * CM_TILE_PX + 4);
    assert(!duke_test_player_tile_solid());

    /* Deterministic scenario, run first while position is still pristine:
     * the "nice girls" item sits just east-northeast of the starting piazza
     * (cm_item_points[7] = (108,109) tiles vs. start (104,110)). Heading
     * turns 1/32 turn per frame while held (a full spin in ~1s), so -- like
     * a real player tapping the stick -- we steer briefly onto a heading
     * near due east/heading index 8, then hold straight, rather than
     * holding RIGHT the whole time (which would just spin in circles). */
    for (int i = 0; i < 9; ++i) {
        g_test_input = BTN_UP | BTN_RIGHT;
        dukes_update();
        dukes_draw();
        check_invariants(i);
    }
    for (int i = 0; i < 81; ++i) {
        g_test_input = BTN_UP;
        dukes_update();
        dukes_draw();
        check_invariants(i);
    }
    printf("targeted pickup drive: player=(%d,%d) collected=%d/8\n",
          duke_test_player_x(), duke_test_player_y(), duke_test_collected_count());
    assert(duke_test_collected_count() >= 1);

    /* Drive straight for a while: exercises axis-separated collision against
     * the promenade/sea boundary just south of the starting piazza. */
    for (int i = 0; i < 400; ++i) {
        g_test_input = BTN_UP;
        dukes_update();
        dukes_draw();
        check_invariants(i);
    }
    printf("after driving straight: player=(%d,%d) fuel=%d\n",
          duke_test_player_x(), duke_test_player_y(), duke_test_fuel());

    /* Heavy randomized fuzzing across several simulated play sessions
     * (restarts happen inside run_fuzz via the rare START press). */
    run_fuzz(60000);
    printf("fuzz complete: state=%d fuel=%d trouble=%d collected=%d/8\n",
          duke_test_state(), duke_test_fuel(), duke_test_trouble(),
          duke_test_collected_count());

    /* Deliberately drive into a wall repeatedly (long stretch of BUILDING
     * tiles is guaranteed one tile off of any cardo alley) and confirm we
     * never tunnel through it. Whatever state fuzzing left us in, get back
     * to PLAYING without assuming a reset (see ensure_playing()). */
    ensure_playing();
    for (int i = 0; i < 2000; ++i) {
        g_test_input = (i & 1) ? (BTN_UP | BTN_LEFT) : (BTN_UP | BTN_RIGHT);
        dukes_update();
        dukes_draw();
        check_invariants(i);
    }
    printf("wall-bash complete: player=(%d,%d) still not inside solid tile\n",
          duke_test_player_x(), duke_test_player_y());

    printf("ALL HOST HARNESS CHECKS PASSED\n");
    return 0;
}
