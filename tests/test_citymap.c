/* Host unit tests for the pure map logic (no engine dependency).
 *   cc -Wall -Wextra -std=c11 citymap.c tests/test_citymap.c -o /tmp/t && /tmp/t
 */
#include "../citymap.h"
#include <assert.h>
#include <stdio.h>

#define W (CM_WORLD_COLS * CM_TILE_PX)
#define H (CM_WORLD_ROWS * CM_TILE_PX)

static unsigned char seen[H][W];
static int queue_x[W * H], queue_y[W * H];

/* Flood fill of every pixel the car's collision box can occupy, starting
 * from the spawn point: the exact set of positions a player can drive to. */
static int flood_from_start(void) {
    static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    int head = 0, tail = 0;
    int sx = cm_start_point.x * CM_TILE_PX + 4, sy = cm_start_point.y * CM_TILE_PX + 4;
    assert(!cm_box_blocked(sx, sy));
    queue_x[tail] = sx; queue_y[tail++] = sy; seen[sy][sx] = 1;
    while (head < tail) {
        int x = queue_x[head], y = queue_y[head++];
        for (int i = 0; i < 4; ++i) {
            int nx = x + dx[i], ny = y + dy[i];
            if (nx < 0 || ny < 0 || nx >= W || ny >= H || seen[ny][nx]) continue;
            if (cm_box_blocked(nx, ny)) continue;
            seen[ny][nx] = 1;
            queue_x[tail] = nx; queue_y[tail++] = ny;
        }
    }
    return tail;
}

static int reachable(cm_point_t p) {
    return seen[p.y * CM_TILE_PX + 4][p.x * CM_TILE_PX + 4];
}

static void test_bounds_are_sea(void) {
    assert(cm_tile_at(-1, 5) == CM_T_SEA);
    assert(cm_tile_at(5, -1) == CM_T_SEA);
    assert(cm_tile_at(CM_WORLD_COLS, 5) == CM_T_SEA);
    assert(cm_tile_at(5, CM_WORLD_ROWS) == CM_T_SEA);
    for (int tx = 0; tx < CM_WORLD_COLS; ++tx)
        assert(cm_tile_at(tx, CM_WORLD_ROWS - 1) == CM_T_SEA);
}

static void test_coast_has_the_shape_of_the_gulf(void) {
    /* The Posillipo promontory reaches further south than both bays. */
    assert(cm_coast_row(62) > cm_coast_row(0) + 16);
    assert(cm_coast_row(62) > cm_coast_row(110) + 20);
    /* Santa Lucia sticks out of Via Caracciolo; the east coast falls away. */
    assert(cm_coast_row(142) > cm_coast_row(120) && cm_coast_row(142) > cm_coast_row(150));
    assert(cm_coast_row(219) > cm_coast_row(176) + 10);
    for (int tx = 0; tx < CM_WORLD_COLS; ++tx) {
        int coast = cm_coast_row(tx);
        assert(coast > 90 && coast < CM_WORLD_ROWS - 4);
        /* Nothing but a named place stands in the sea, and the shore itself
         * is always the lungomare or a named place. */
        uint8_t shore = cm_tile_at(tx, coast - 1);
        assert(shore == CM_T_PROMENADE || shore == CM_T_PARTY || shore == CM_T_PIAZZA ||
               shore == CM_T_PARK || shore == CM_T_LANDMARK);
    }
    /* Castel dell'Ovo stands on its islet, joined by the causeway. */
    assert(cm_tile_at(142, 109) == CM_T_PIAZZA && cm_tile_at(137, 110) == CM_T_SEA);
    assert(cm_tile_at(142, 112) == CM_T_LANDMARK);
    assert(cm_tile_at(141, 105) == CM_T_PROMENADE);
}

static void test_streets_fit_the_car(void) {
    /* The box is exactly one tile wide, so both street widths leave room. */
    assert(2 * CM_CAR_HALF <= CM_TILE_PX);
    assert(CM_CARDO_WIDTH * CM_TILE_PX >= 2 * CM_CAR_HALF + 4);
    assert(CM_DECUMANO_WIDTH * CM_TILE_PX >= 2 * CM_CAR_HALF + 4);
    /* A plain cardo of the centro storico, away from zones. */
    assert(cm_tile_at(110, 40) == CM_T_ROAD && cm_tile_at(111, 40) == CM_T_ROAD);
    assert(cm_tile_is_solid(cm_tile_at(112, 39)));
    assert(!cm_box_blocked(110 * 8 + 8, 40 * 8 + 4));
    /* Blocks are wider in Fuorigrotta than in the centro storico. */
    assert(cm_tile_is_solid(cm_tile_at(8, 40)) && cm_tile_at(12, 40) == CM_T_ROAD);
}

static void test_zones_resolve(void) {
    for (int i = 0; i < cm_zone_count; ++i) {
        const cm_zone_t *z = &cm_zones[i];
        assert(z->x >= 0 && z->y >= 0 && z->x + z->w <= CM_WORLD_COLS);
        assert(z->y + z->h < CM_WORLD_ROWS);
        /* No zone is completely hidden behind an earlier one. */
        int shown = 0;
        for (int ty = z->y; ty < z->y + z->h; ++ty)
            for (int tx = z->x; tx < z->x + z->w; ++tx)
                if (cm_tile_at(tx, ty) == z->tile) shown++;
        assert(shown * 2 >= z->w * z->h);
    }
}

static void test_points_of_interest(void) {
    for (int i = 0; i < CM_POI_COUNT; ++i) {
        const cm_poi_t *p = &cm_pois[i];
        int around = 0;
        assert(p->x >= 0 && p->y >= 0 && p->x + p->w <= CM_WORLD_COLS && p->y + p->h < CM_WORLD_ROWS);
        for (int ty = p->y; ty < p->y + p->h; ++ty)
            for (int tx = p->x; tx < p->x + p->w; ++tx) {
                if (p->style != CM_POI_PLACE) {
                    /* A monument stands whole: solid, on what was a city block
                     * or a named place, never across a street of the grid. */
                    assert(cm_tile_at(tx, ty) == CM_T_LANDMARK);
                    assert(cm_poi_near(tx, ty) == i);
                }
            }
        /* Every one can be driven up to: some reachable tile is within its
         * surroundings, and there it is the place announced. */
        for (int ty = p->y - 3; ty < p->y + p->h + 3; ++ty)
            for (int tx = p->x - 3; tx < p->x + p->w + 3; ++tx) {
                if (tx < 0 || ty < 0 || tx >= CM_WORLD_COLS || ty >= CM_WORLD_ROWS) continue;
                if (cm_tile_is_solid(cm_tile_at(tx, ty))) continue;
                if (seen[ty * CM_TILE_PX + 4][tx * CM_TILE_PX + 4] && cm_poi_near(tx, ty) == i) around++;
            }
        if (!around) { fprintf(stderr, "point of interest %d cannot be visited\n", i); assert(0); }
    }
    assert(cm_poi_near(cm_start_point.x, cm_start_point.y) == CM_AT_ROCK_GARDEN);
    /* The Rock Garden stands a few doors from the Rettifilo, and the Duchesca
     * is a short drive east of it. */
    assert(cm_pois[CM_AT_DUCHESCA].x - cm_start_point.x < 24);
    assert(cm_tile_at(152, 84) == CM_T_ROAD && 84 - cm_start_point.y < 10);
    assert(cm_poi_near(cm_party_point.x, cm_party_point.y) == CM_AT_VILLA_DORIA);
    assert(cm_poi_near(2, 30) == -1);
}

static void test_landmarks_are_reachable(void) {
    assert(cm_tile_at(cm_start_point.x, cm_start_point.y) == CM_T_ROAD);
    for (int i = 0; i < CM_CHECKPOINT_COUNT; ++i) {
        /* A checkpoint lies on plain street, spans its whole width, and can
         * be reached; the start is not inside one. */
        const cm_poi_t *c = &cm_checkpoints[i];
        for (int ty = c->y; ty < c->y + c->h; ++ty)
            for (int tx = c->x; tx < c->x + c->w; ++tx) {
                uint8_t t = cm_tile_at(tx, ty);
                assert(t == CM_T_ROAD || t == CM_T_ROAD_LINE);
                assert(cm_checkpoint_at(tx, ty) == i);
                assert(seen[ty * CM_TILE_PX + 4][tx * CM_TILE_PX + 4]);
            }
        if (c->h == 3) assert(cm_tile_is_solid(cm_tile_at(c->x, c->y - 1)) && cm_tile_is_solid(cm_tile_at(c->x, c->y + 3)));
        else assert(cm_tile_is_solid(cm_tile_at(c->x - 1, c->y)) && cm_tile_is_solid(cm_tile_at(c->x + 2, c->y)));
    }
    assert(cm_checkpoint_at(cm_start_point.x, cm_start_point.y) == -1);
    assert(cm_tile_at(cm_party_point.x, cm_party_point.y) == CM_T_PARTY);
    assert(reachable(cm_party_point));
    for (int i = 0; i < CM_ITEM_COUNT; ++i) {
        assert(!cm_tile_is_solid(cm_tile_at(cm_item_points[i].x, cm_item_points[i].y)));
        if (!reachable(cm_item_points[i])) {
            fprintf(stderr, "item %d is not reachable from the start\n", i);
            assert(0);
        }
    }
    for (int i = 0; i < CM_RAUTI_COUNT; ++i) {
        assert(!cm_tile_is_solid(cm_tile_at(cm_rauti_points[i].x, cm_rauti_points[i].y)));
        assert(reachable(cm_rauti_points[i]));
    }
    for (int i = 0; i < CM_GAS_COUNT; ++i) {
        assert(cm_tile_at(cm_gas_points[i].x, cm_gas_points[i].y) == CM_T_GAS);
        assert(reachable(cm_gas_points[i]));
    }
}

static void test_every_open_tile_is_connected(void) {
    /* No drivable pocket is cut off from the rest of the city: enemies can
     * spawn on any open tile and still find the player. */
    for (int ty = 0; ty < CM_WORLD_ROWS; ++ty) {
        for (int tx = 0; tx < CM_WORLD_COLS; ++tx) {
            if (cm_tile_is_solid(cm_tile_at(tx, ty))) continue;
            int px = tx * CM_TILE_PX + 4, py = ty * CM_TILE_PX + 4;
            if (!seen[py][px]) {
                fprintf(stderr, "open tile (%d,%d) is cut off\n", tx, ty);
                assert(0);
            }
        }
    }
}

static void test_trig_tables(void) {
    for (int i = 0; i < 32; ++i) {
        int c = cm_cos_table[i], s = cm_sin_table[i];
        int mag = c * c + s * s;
        assert(mag > 63000 && mag < 68000); /* ~256^2 */
    }
    assert(cm_cos_table[0] == 0 && cm_sin_table[0] == -256);  /* north */
    assert(cm_cos_table[8] == 256 && cm_sin_table[8] == 0);   /* east */
}

int main(void) {
    int open_pixels = flood_from_start();
    test_bounds_are_sea();
    test_coast_has_the_shape_of_the_gulf();
    test_streets_fit_the_car();
    test_zones_resolve();
    test_points_of_interest();
    test_landmarks_are_reachable();
    test_every_open_tile_is_connected();
    test_trig_tables();
    printf("citymap: %d reachable car positions, all landmarks connected\n", open_pixels);
    printf("ALL CITYMAP TESTS PASSED\n");
    return 0;
}
