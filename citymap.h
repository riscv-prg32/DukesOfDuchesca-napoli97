#ifndef DUKES_CITYMAP_H
#define DUKES_CITYMAP_H

/*
 * Pure, freestanding-safe city-layout logic for DukesOfDuchesca-napoli97.
 *
 * This header has NO dependency on prg32.h so it can be unit tested with a
 * plain host compiler (see tests/test_citymap.c) as well as compiled into
 * the RISC-V cartridge. It procedurally derives a Naples-flavoured street
 * grid (long "decumani" crossed by narrow "cardini", a Gulf-of-Naples
 * coastline, named piazzas) that is many times larger than the 320x200
 * viewport, without storing a giant tile array: every tile is computed on
 * demand from a handful of small landmark tables plus modulo arithmetic.
 */

#include <stdint.h>

#define CM_TILE_PX 8

/* World size in tiles. 220x130 tiles = 1760x1040 px, roughly 5.5x the
 * 320px-wide viewport and 5.2x the 200px-tall viewport (~29x the area). */
#define CM_WORLD_COLS 220
#define CM_WORLD_ROWS 130

/* Layer-1 (city) tile ids. Id 0 is treated as transparent by the engine's
 * dual-playfield draw, so CM_T_SEA doubles as the "show the backdrop layer"
 * marker: wherever the city is water, the slow-parallax Gulf/Vesuvio
 * backdrop on layer 0 shows through. */
#define CM_T_SEA 0
#define CM_T_ROAD 1
#define CM_T_ROAD_LINE 2
#define CM_T_BUILDING 3
#define CM_T_PIAZZA 4
#define CM_T_PARK 5
#define CM_T_GAS 6
#define CM_T_PARTY 7
#define CM_T_PROMENADE 8
#define CM_TILE_COUNT 9

/* Layer-0 (backdrop) tile ids: flat gulf water and a two-tile Vesuvio
 * silhouette (twin peaks: Gran Cono + Monte Somma), repeated periodically. */
#define CM_B_SEA 0
#define CM_B_VESUVIO_L 1
#define CM_B_VESUVIO_R 2

typedef struct {
    int x, y, w, h;
    uint8_t tile;
} cm_zone_t;

typedef struct {
    int x, y;
} cm_point_t;

#define CM_ITEM_COUNT 8

/* Named piazzas and other rectangular overrides, all in tile coordinates. */
extern const cm_zone_t cm_zones[];
extern const int cm_zone_count;

/* The player's starting piazza (Plebiscito, by the water) and the party
 * destination (a villa up in Posillipo, across the whole city). */
extern const cm_point_t cm_start_point;
extern const cm_point_t cm_party_point;

/* Gas stations scattered along the road network. */
extern const cm_point_t cm_gas_points[];
extern const int cm_gas_point_count;

/* One spawn point per party item, scattered across the piazzas. */
extern const cm_point_t cm_item_points[CM_ITEM_COUNT];

/* Returns the layer-1 city tile id for a tile coordinate. Out-of-range
 * coordinates return CM_T_SEA so the playable city is bounded by water. */
uint8_t cm_tile_at(int tx, int ty);

/* Returns the layer-0 backdrop tile id for a physical playfield cell
 * (0..63, 0..31): a fixed decorative pattern, independent of world scroll. */
uint8_t cm_backdrop_at(int cell_x, int cell_y);

/* True if a layer-1 tile id blocks vehicle movement. */
int cm_tile_is_solid(uint8_t tile);

/* 32-step cosine/sine lookup, Q8 fixed point (-256..256), used for the car's
 * heading. Angle indices wrap modulo 32. */
extern const int16_t cm_cos_table[32];
extern const int16_t cm_sin_table[32];

#endif
