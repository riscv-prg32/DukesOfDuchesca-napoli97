#include "citymap.h"

/* Sea band along the southern edge of the world (the Gulf of Naples), with a
 * two-tile promenade (a stand-in for Via Caracciolo) running right above the
 * waterline. */
#define CM_SEA_ROWS 14
#define CM_PROM_ROWS 2
#define CM_SEA_TOP (CM_WORLD_ROWS - CM_SEA_ROWS)
#define CM_PROM_TOP (CM_SEA_TOP - CM_PROM_ROWS)

/* Historic-center street grid: wide decumani running east-west, crossed by
 * many narrow cardini running north-south. */
#define CM_DECUMANO_SPACING 16
#define CM_DECUMANO_WIDTH 2
#define CM_CARDO_SPACING 6

/* Named piazzas, a park, and the party destination, all as tile-space
 * rectangles checked before the procedural grid. The first matching zone
 * wins. */
const cm_zone_t cm_zones[] = {
    {100, 106, 10, 8, CM_T_PIAZZA},  /* Piazza del Plebiscito (start) */
    {148, 58, 8, 8, CM_T_PIAZZA},    /* Piazza Garibaldi */
    {58, 48, 6, 6, CM_T_PIAZZA},     /* Piazza Dante */
    {128, 88, 6, 5, CM_T_PIAZZA},    /* Piazza dei Martiri */
    {38, 28, 5, 5, CM_T_PIAZZA},     /* Piazza Bellini */
    {178, 93, 6, 6, CM_T_PIAZZA},    /* Piazza Mercato */
    {88, 38, 5, 5, CM_T_PIAZZA},     /* Largo Corpo di Napoli */
    {112, 98, 12, 5, CM_T_PARK},     /* Villa Comunale gardens */
    {14, 14, 8, 7, CM_T_PARTY},      /* the Posillipo party villa */
    /* Gas station patches, centered on cm_gas_points below. */
    {47, 89, 3, 3, CM_T_GAS}, {173, 41, 3, 3, CM_T_GAS},
    {29, 71, 3, 3, CM_T_GAS}, {143, 19, 3, 3, CM_T_GAS},
    {191, 111, 3, 3, CM_T_GAS}, {77, 17, 3, 3, CM_T_GAS},
};
const int cm_zone_count = (int)(sizeof(cm_zones) / sizeof(cm_zones[0]));

const cm_point_t cm_start_point = {104, 110};
const cm_point_t cm_party_point = {18, 17};

/* Every point below sits on a cardo column (x is a multiple of 6), which
 * guarantees a drivable road tile unless a named zone overrides it -- see
 * test_all_gas_points_are_drivable. */
const cm_point_t cm_gas_points[] = {
    {48, 90}, {174, 42}, {30, 72}, {144, 20}, {192, 112}, {78, 18},
};
const int cm_gas_point_count =
    (int)(sizeof(cm_gas_points) / sizeof(cm_gas_points[0]));

const cm_point_t cm_item_points[CM_ITEM_COUNT] = {
    {41, 31},   /* beer: Piazza Bellini */
    {61, 51},   /* wine: Piazza Dante */
    {91, 41},   /* Sangria Papelis: Largo Corpo di Napoli */
    {152, 62},  /* amplifier: Piazza Garibaldi */
    {131, 91},  /* loudspeaker: Piazza dei Martiri */
    {181, 96},  /* mixer: Piazza Mercato */
    {116, 96},  /* disco lights: just off Villa Comunale */
    {108, 109}, /* nice girls: Piazza del Plebiscito */
};

static uint32_t cm_hash(int tx, int ty) {
    uint32_t h = (uint32_t)tx * 374761393u + (uint32_t)ty * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return h;
}

static int cm_in_zone(const cm_zone_t *z, int tx, int ty) {
    return tx >= z->x && tx < z->x + z->w && ty >= z->y && ty < z->y + z->h;
}

uint8_t cm_tile_at(int tx, int ty) {
    if (tx < 0 || ty < 0 || tx >= CM_WORLD_COLS || ty >= CM_WORLD_ROWS) {
        return CM_T_SEA;
    }

    for (int i = 0; i < cm_zone_count; ++i) {
        if (cm_in_zone(&cm_zones[i], tx, ty)) {
            return cm_zones[i].tile;
        }
    }

    if (ty >= CM_SEA_TOP) {
        return CM_T_SEA;
    }
    if (ty >= CM_PROM_TOP) {
        return CM_T_PROMENADE;
    }

    int decumano_row = ty % CM_DECUMANO_SPACING;
    if (decumano_row < CM_DECUMANO_WIDTH) {
        return ((tx + decumano_row) & 3) == 0 ? CM_T_ROAD_LINE : CM_T_ROAD;
    }
    if ((tx % CM_CARDO_SPACING) == 0) {
        return CM_T_ROAD;
    }

    return (cm_hash(tx, ty) % 23u) == 0 ? CM_T_PARK : CM_T_BUILDING;
}

int cm_tile_is_solid(uint8_t tile) {
    return tile == CM_T_BUILDING || tile == CM_T_PARK || tile == CM_T_SEA;
}

uint8_t cm_backdrop_at(int cell_x, int cell_y) {
    int col = ((cell_x % 20) + 20) % 20;
    int row = ((cell_y % 32) + 32) % 32;
    if (row >= 18 && row <= 21) {
        if (col == 9) return CM_B_VESUVIO_L;
        if (col == 10) return CM_B_VESUVIO_R;
    }
    return CM_B_SEA;
}

/* Q8 fixed point, 32 steps around the circle, index 0 pointing "up" (north)
 * so a heading of 0 matches an unrotated car sprite. */
const int16_t cm_cos_table[32] = {
    0,    50,   98,   142,  181,  213,  237,  251,  256,  251,  237,  213,
    181,  142,  98,   50,   0,    -50,  -98,  -142, -181, -213, -237, -251,
    -256, -251, -237, -213, -181, -142, -98,  -50,
};
const int16_t cm_sin_table[32] = {
    -256, -251, -237, -213, -181, -142, -98,  -50,  0,    50,   98,   142,
    181,  213,  237,  251,  256,  251,  237,  213,  181,  142,  98,   50,
    0,    -50,  -98,  -142, -181, -213, -237, -251,
};
