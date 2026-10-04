#include "citymap.h"

/* The coastline as (column, first sea row) corner points, west to east. */
static const uint8_t cm_coast_points[][2] = {
    {0, 100},   /* Bagnoli */
    {30, 104},
    {45, 112},  /* towards Nisida */
    {62, 124},  /* Capo Posillipo */
    {74, 122},
    {84, 110},  /* Via Posillipo climbing back north-east */
    {96, 98},   /* Mergellina */
    {110, 96},  /* Via Caracciolo */
    {136, 99},
    {142, 104}, /* Santa Lucia */
    {148, 99},  /* Molo Beverello, then the port */
    {176, 100},
    {200, 110}, /* San Giovanni a Teduccio */
    {219, 118},
};
#define CM_COAST_POINTS (int)(sizeof(cm_coast_points) / sizeof(cm_coast_points[0]))

static uint8_t cm_coast[CM_WORLD_COLS];
static uint8_t cm_coast_ready;

int cm_coast_row(int tx) {
    if (!cm_coast_ready) {
        for (int i = 0; i + 1 < CM_COAST_POINTS; ++i) {
            int x0 = cm_coast_points[i][0], y0 = cm_coast_points[i][1];
            int x1 = cm_coast_points[i + 1][0], y1 = cm_coast_points[i + 1][1];
            for (int x = x0; x <= x1; ++x)
                cm_coast[x] = (uint8_t)(y0 + (y1 - y0) * (x - x0) / (x1 - x0));
        }
        cm_coast_ready = 1;
    }
    if (tx < 0) tx = 0;
    if (tx >= CM_WORLD_COLS) tx = CM_WORLD_COLS - 1;
    return cm_coast[tx];
}

/* Named places as tile-space rectangles, checked before everything else
 * (so they can also stand in the sea). The first matching zone wins. Every
 * drivable zone touches a street; reachability of every landmark is proven
 * by tests/test_citymap.c. */
const cm_zone_t cm_zones[] = {
    {132, 88, 10, 7, CM_T_PIAZZA},    /* Piazza del Plebiscito (start) */
    {176, 66, 10, 8, CM_T_PIAZZA},    /* Piazza Garibaldi, Stazione Centrale */
    {124, 66, 6, 6, CM_T_PIAZZA},     /* Piazza Dante, on Via Toledo */
    {134, 54, 5, 5, CM_T_PIAZZA},     /* Piazza Bellini */
    {146, 60, 6, 5, CM_T_PIAZZA},     /* Largo Corpo di Napoli */
    {112, 84, 6, 4, CM_T_PIAZZA},     /* Piazza dei Martiri, Chiaia */
    {166, 84, 8, 6, CM_T_PIAZZA},     /* Piazza Mercato */
    {84, 60, 6, 6, CM_T_PIAZZA},      /* Piazza Vanvitelli, Vomero */
    {33, 73, 6, 4, CM_T_PARK},        /* the pitch of the Stadio San Paolo... */
    {30, 70, 12, 10, CM_T_PIAZZA},    /* ...and its stands, Fuorigrotta */
    {139, 108, 6, 5, CM_T_PIAZZA},    /* Castel dell'Ovo, on its islet */
    {141, 103, 2, 5, CM_T_PROMENADE}, /* the causeway of Borgo Marinari */
    {154, 99, 3, 8, CM_T_PROMENADE},  /* Molo Beverello */
    {106, 88, 20, 4, CM_T_PARK},      /* Villa Comunale */
    {60, 108, 10, 8, CM_T_PARTY},     /* the party villa at Posillipo */
    {58, 116, 14, 3, CM_T_PARK},      /* Parco Virgiliano */
    {128, 12, 2, 4, CM_T_ROAD},       /* the drive up to Capodimonte */
    {126, 8, 8, 4, CM_T_PIAZZA},      /* the Reggia di Capodimonte... */
    {120, 4, 22, 12, CM_T_PARK},      /* ...in its woods */
    {62, 3, 34, 10, CM_T_PARK},       /* the hill of the Camaldoli */
    {178, 10, 32, 2, CM_T_ROAD},      /* Capodichino: the runway... */
    {176, 6, 36, 10, CM_T_PIAZZA},    /* ...and the apron */
    /* Gas station forecourts: a 3x3 patch in the corner of a block, right
     * next to a crossroads. Centres are listed in cm_gas_points. */
    {26, 83, 3, 3, CM_T_GAS}, {78, 51, 3, 3, CM_T_GAS},
    {98, 83, 3, 3, CM_T_GAS}, {142, 67, 3, 3, CM_T_GAS},
    {190, 83, 3, 3, CM_T_GAS}, {112, 19, 3, 3, CM_T_GAS},
    {38, 51, 3, 3, CM_T_GAS}, {78, 99, 3, 3, CM_T_GAS},
};
const int cm_zone_count = (int)(sizeof(cm_zones) / sizeof(cm_zones[0]));

const cm_point_t cm_start_point = {136, 91};
const cm_point_t cm_party_point = {65, 112};

const cm_point_t cm_gas_points[CM_GAS_COUNT] = {
    {27, 84}, {79, 52}, {99, 84}, {143, 68}, {191, 84}, {113, 20}, {39, 52}, {79, 100},
};

const cm_point_t cm_item_points[CM_ITEM_COUNT] = {
    {136, 56},  /* beer: Piazza Bellini */
    {127, 69},  /* wine: Piazza Dante */
    {149, 62},  /* Sangria Papelis: Largo Corpo di Napoli */
    {181, 70},  /* amplifier: Piazza Garibaldi */
    {35, 71},   /* loudspeakers: the Stadio San Paolo */
    {170, 87},  /* mixer: Piazza Mercato */
    {87, 63},   /* disco lights: Piazza Vanvitelli */
    {142, 110}, /* nice girls: Borgo Marinari, under Castel dell'Ovo */
};

uint32_t cm_hash(int tx, int ty) {
    uint32_t h = (uint32_t)tx * 374761393u + (uint32_t)ty * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return h;
}

/* Districts, west to east: where each starts and how far apart its cardini
 * are. Every district opens with a cardo, so the grids join up. */
static int cm_cardo_col(int tx, int *block) {
    int origin, spacing;
    if (tx < 56) { origin = 0; spacing = 12; }         /* Bagnoli, Fuorigrotta */
    else if (tx < 104) { origin = 56; spacing = 10; }  /* Posillipo, Vomero, Chiaia */
    else if (tx < 160) { origin = 104; spacing = 6; }  /* centro storico */
    else { origin = 160; spacing = 14; }               /* station, industrial east */
    if (block) *block = origin + (tx - origin) / spacing;
    return (tx - origin) % spacing;
}

int cm_block_style(int tx, int ty) {
    int block;
    cm_cardo_col(tx, &block);
    if (tx >= 188) return 3; /* the grey sheds of the industrial zone */
    return (int)(cm_hash(block + 7, ty / CM_DECUMANO_SPACING + 3) >> 5) & 3;
}

uint8_t cm_tile_at(int tx, int ty) {
    if (tx < 0 || ty < 0 || tx >= CM_WORLD_COLS || ty >= CM_WORLD_ROWS) {
        return CM_T_SEA;
    }

    for (int i = 0; i < cm_zone_count; ++i) {
        const cm_zone_t *z = &cm_zones[i];
        if (tx >= z->x && tx < z->x + z->w && ty >= z->y && ty < z->y + z->h) {
            return z->tile;
        }
    }

    if (ty >= cm_coast_row(tx)) {
        return CM_T_SEA;
    }
    /* The lungomare: land with the sea within three tiles, below or beside. */
    if (ty + CM_PROM_DEPTH >= cm_coast_row(tx) || ty >= cm_coast_row(tx - CM_PROM_DEPTH) ||
        ty >= cm_coast_row(tx + CM_PROM_DEPTH)) {
        return CM_T_PROMENADE;
    }

    /* Corso Umberto, the "Rettifilo": the one diagonal, cut from Piazza
     * Garibaldi down to the port. One row south every two columns west. */
    if (tx >= 150 && tx < 176) {
        int row = 72 + (176 - tx) / 2;
        if (ty == row || ty == row + 1) return CM_T_ROAD;
    }

    int cardo_col = cm_cardo_col(tx, 0);
    int decumano_row = ty % CM_DECUMANO_SPACING;
    if (decumano_row < CM_DECUMANO_WIDTH) {
        /* Dashed centre line, interrupted at the crossroads. */
        if (decumano_row == 1 && cardo_col >= CM_CARDO_WIDTH && (tx & 1) == 0) {
            return CM_T_ROAD_LINE;
        }
        return CM_T_ROAD;
    }
    if (cardo_col < CM_CARDO_WIDTH) {
        return CM_T_ROAD;
    }
    /* The centro storico has three decumani where the rest of the city has
     * one: two more narrow ones between each pair (Spaccanapoli is one). */
    if (tx >= 104 && tx < 160 && ty >= 36 && ty < 92 && (decumano_row == 8 || decumano_row == 9)) {
        return CM_T_ROAD;
    }

    /* A few courtyard gardens inside the blocks. */
    return (cm_hash(tx, ty) % 29u) == 0 ? CM_T_PARK : CM_T_BUILDING;
}

int cm_tile_is_solid(uint8_t tile) {
    return tile == CM_T_BUILDING || tile == CM_T_PARK || tile == CM_T_SEA;
}

/* Arithmetic shift: negative pixels map to negative tiles, which are sea. */
int cm_box_blocked(int px, int py) {
    int tx0 = (px - CM_CAR_HALF) >> 3, tx1 = (px + CM_CAR_HALF - 1) >> 3;
    int ty0 = (py - CM_CAR_HALF) >> 3, ty1 = (py + CM_CAR_HALF - 1) >> 3;
    return cm_tile_is_solid(cm_tile_at(tx0, ty0)) ||
           cm_tile_is_solid(cm_tile_at(tx1, ty0)) ||
           cm_tile_is_solid(cm_tile_at(tx0, ty1)) ||
           cm_tile_is_solid(cm_tile_at(tx1, ty1));
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
