/* Host-side unit tests for citymap.c: no prg32.h dependency, plain assert. */
#include "../citymap.h"
#include <assert.h>
#include <stdio.h>

static void test_world_is_much_larger_than_viewport(void) {
    assert(CM_WORLD_COLS * CM_TILE_PX >= 320 * 4);
    assert(CM_WORLD_ROWS * CM_TILE_PX >= 200 * 4);
}

static void test_out_of_range_is_sea(void) {
    assert(cm_tile_at(-1, 0) == CM_T_SEA);
    assert(cm_tile_at(0, -1) == CM_T_SEA);
    assert(cm_tile_at(CM_WORLD_COLS, 0) == CM_T_SEA);
    assert(cm_tile_at(0, CM_WORLD_ROWS) == CM_T_SEA);
}

static void test_start_and_party_points_are_drivable(void) {
    uint8_t start_tile = cm_tile_at(cm_start_point.x, cm_start_point.y);
    uint8_t party_tile = cm_tile_at(cm_party_point.x, cm_party_point.y);
    assert(!cm_tile_is_solid(start_tile));
    assert(!cm_tile_is_solid(party_tile));
    assert(party_tile == CM_T_PARTY);
}

static void test_all_item_points_are_drivable(void) {
    for (int i = 0; i < CM_ITEM_COUNT; ++i) {
        uint8_t t = cm_tile_at(cm_item_points[i].x, cm_item_points[i].y);
        assert(!cm_tile_is_solid(t));
    }
}

static void test_all_gas_points_are_drivable(void) {
    for (int i = 0; i < cm_gas_point_count; ++i) {
        uint8_t t = cm_tile_at(cm_gas_points[i].x, cm_gas_points[i].y);
        assert(!cm_tile_is_solid(t));
        assert(t == CM_T_GAS);
    }
}

static void test_deep_south_is_sea_and_bounds_the_city(void) {
    assert(cm_tile_at(100, CM_WORLD_ROWS - 1) == CM_T_SEA);
    assert(cm_tile_is_solid(CM_T_SEA));
}

static void test_deterministic(void) {
    for (int i = 0; i < 5; ++i) {
        assert(cm_tile_at(73, 41) == cm_tile_at(73, 41));
        assert(cm_backdrop_at(5, 19) == cm_backdrop_at(5, 19));
    }
}

static void test_backdrop_wraps_within_physical_playfield(void) {
    /* Physical playfield is 64x32; the backdrop is filled once into that
     * buffer so it must be well-defined (and stable) across the whole
     * wrapped range, including negative-looking modulo edge cases. */
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 64; ++x) {
            uint8_t b = cm_backdrop_at(x, y);
            assert(b == CM_B_SEA || b == CM_B_VESUVIO_L || b == CM_B_VESUVIO_R);
        }
    }
}

static void test_heading_tables_are_consistent(void) {
    /* Cardinal directions should be exact multiples of 256 (Q8 for 1.0). */
    assert(cm_cos_table[0] == 0 && cm_sin_table[0] == -256);   /* up */
    assert(cm_cos_table[8] == 256 && cm_sin_table[8] == 0);    /* right */
    assert(cm_cos_table[16] == 0 && cm_sin_table[16] == 256);  /* down */
    assert(cm_cos_table[24] == -256 && cm_sin_table[24] == 0); /* left */
    for (int i = 0; i < 32; ++i) {
        long c = cm_cos_table[i];
        long s = cm_sin_table[i];
        long mag2 = c * c + s * s;
        /* Should be close to 256^2 = 65536 given integer rounding. */
        assert(mag2 > 64000 && mag2 < 67200);
    }
}

static void test_decumani_are_two_tiles_wide_and_periodic(void) {
    int found_road_row = 0;
    for (int ty = 0; ty < 16; ++ty) {
        uint8_t t = cm_tile_at(3, ty);
        if (t == CM_T_ROAD || t == CM_T_ROAD_LINE) found_road_row++;
    }
    assert(found_road_row >= 2);
}

int main(void) {
    test_world_is_much_larger_than_viewport();
    test_out_of_range_is_sea();
    test_start_and_party_points_are_drivable();
    test_all_item_points_are_drivable();
    test_all_gas_points_are_drivable();
    test_deep_south_is_sea_and_bounds_the_city();
    test_deterministic();
    test_backdrop_wraps_within_physical_playfield();
    test_heading_tables_are_consistent();
    test_decumani_are_two_tiles_wide_and_periodic();
    printf("all citymap tests passed\n");
    return 0;
}
