#ifndef DUKES_CITYMAP_H
#define DUKES_CITYMAP_H

/*
 * Pure, freestanding-safe city-layout logic for DukesOfDuchesca-napoli97.
 *
 * This header has NO dependency on prg32.h so it can be unit tested with a
 * plain host compiler (see tests/test_citymap.c) as well as compiled into
 * the RISC-V cartridge. It procedurally derives a stylised Napoli, laid out
 * after the overview plate of a city street atlas: the coast of the gulf
 * with Posillipo, Mergellina, Santa Lucia and the port; Fuorigrotta, the
 * Vomero, the centro storico, the station and the industrial east; the
 * woods of Capodimonte and the airport of Capodichino in the north. It is
 * many times larger than the 320x200 viewport and stores no tile array:
 * every tile is computed on demand from small tables plus arithmetic. It is
 * a drawing from memory, not a survey: nothing is to scale.
 */

#include <stdint.h>

#define CM_TILE_PX 8

/* World size in tiles. 220x130 tiles = 1760x1040 px, roughly 5.5x the
 * 320px-wide viewport and 5.2x the 200px-tall viewport (~29x the area). */
#define CM_WORLD_COLS 220
#define CM_WORLD_ROWS 130

/* The coastline follows the shape of the real city around the gulf: the
 * bay of Bagnoli in the west, the Posillipo promontory, Mergellina and Via
 * Caracciolo, the bump of Santa Lucia, the port, and the coast falling away
 * south-east towards San Giovanni. cm_coast_row() is the first sea row of a
 * column; a lungomare three tiles deep runs along the whole shore. */
#define CM_PROM_DEPTH 3

/* Streets: decumani run east-west every 16 rows and are three tiles (24 px)
 * wide; cardini run north-south and are two tiles (16 px) wide, at a spacing
 * that depends on the district (wide blocks in Fuorigrotta, the tight Greek
 * grid in the centro storico, sheds in the industrial east). Both are wider
 * than the car (CM_CAR_HALF), which v1's one-tile cardini were not: there
 * the car could never leave the street it started on. */
#define CM_DECUMANO_SPACING 16
#define CM_DECUMANO_WIDTH 3
#define CM_CARDO_WIDTH 2

/* Half side of every vehicle's collision box, in pixels. The box covers
 * [x - CM_CAR_HALF, x + CM_CAR_HALF - 1], i.e. exactly one tile, so testing
 * its four corners can never straddle (and miss) a solid tile. */
#define CM_CAR_HALF 4

/* Tile kinds. */
#define CM_T_SEA 0
#define CM_T_ROAD 1
#define CM_T_ROAD_LINE 2
#define CM_T_BUILDING 3
#define CM_T_PIAZZA 4
#define CM_T_PARK 5
#define CM_T_GAS 6
#define CM_T_PARTY 7
#define CM_T_PROMENADE 8
#define CM_T_LANDMARK 9 /* a monument: solid, drawn by the game in its own style */
#define CM_TILE_COUNT 10

typedef struct {
    int16_t x, y, w, h;
    uint8_t tile;
} cm_zone_t;

typedef struct {
    int16_t x, y;
} cm_point_t;

/* Points of interest: the real places of Napoli a visitor would look for.
 * A place with a `style` other than CM_POI_PLACE is a building standing on
 * its rectangle (solid); a CM_POI_PLACE is only a name for a zone below. */
enum {
    CM_POI_PLACE = 0, CM_POI_CASTLE, CM_POI_STAR_FORT, CM_POI_SEA_CASTLE, CM_POI_PALACE,
    CM_POI_BASILICA, CM_POI_CATHEDRAL, CM_POI_MUSEUM, CM_POI_STATION, CM_POI_GALLERIA,
    CM_POI_TOWERS, CM_POI_FAIR, CM_POI_CLUB,
};
enum {
    CM_AT_PLEBISCITO = 0, CM_AT_SAN_FRANCESCO, CM_AT_PALAZZO_REALE, CM_AT_MASCHIO_ANGIOINO,
    CM_AT_CASTEL_DELL_OVO, CM_AT_SANT_ELMO, CM_AT_SAN_PAOLO, CM_AT_DUOMO, CM_AT_MUSEO,
    CM_AT_CAPODIMONTE, CM_AT_VILLA_DORIA, CM_AT_STAZIONE, CM_AT_GALLERIA,
    CM_AT_CENTRO_DIREZIONALE, CM_AT_MOSTRA, CM_AT_CAPODICHINO, CM_AT_VILLA_COMUNALE,
    CM_AT_BEVERELLO, CM_AT_DANTE, CM_AT_VANVITELLI, CM_AT_MERCATO, CM_AT_MARTIRI,
    CM_AT_ROCK_GARDEN, CM_AT_DUCHESCA,
    CM_POI_COUNT
};

typedef struct {
    int16_t x, y, w, h;
    uint8_t style;
} cm_poi_t;

extern const cm_poi_t cm_pois[CM_POI_COUNT];

/* The point of interest the tile is at or within three tiles of, or -1.
 * Buildings are preferred to the places around them. */
int cm_poi_near(int tx, int ty);

#define CM_ITEM_COUNT 8
#define CM_GAS_COUNT 8
#define CM_RAUTI_COUNT 7

/* Named piazzas and other rectangular overrides, all in tile coordinates. */
extern const cm_zone_t cm_zones[];
extern const int cm_zone_count;

/* Where the night starts (outside the Rock Garden, the rock club of Via San
 * Giovanni Maggiore Pignatelli, in the university quarter by the Rettifilo)
 * and the party destination (Villa Doria d'Angri at Posillipo, across the
 * bay). */
extern const cm_point_t cm_start_point;
extern const cm_point_t cm_party_point;

/* Gas stations: the centre tile of each 3x3 forecourt. */
extern const cm_point_t cm_gas_points[CM_GAS_COUNT];

/* Police checkpoints (posti di blocco): a stretch of street, in tiles. */
#define CM_CHECKPOINT_COUNT 7
extern const cm_poi_t cm_checkpoints[CM_CHECKPOINT_COUNT];

/* The checkpoint a tile is in, or -1. */
int cm_checkpoint_at(int tx, int ty);

/* Boxes of rauti (bangers), one per point. */
extern const cm_point_t cm_rauti_points[CM_RAUTI_COUNT];

/* One spawn point per party item, scattered across the piazzas. */
extern const cm_point_t cm_item_points[CM_ITEM_COUNT];

/* Returns the tile kind for a tile coordinate. Out-of-range coordinates
 * return CM_T_SEA so the playable city is bounded by water. */
uint8_t cm_tile_at(int tx, int ty);

/* First sea row of a column (the coastline). */
int cm_coast_row(int tx);

/* True if a tile kind blocks vehicle movement. */
int cm_tile_is_solid(uint8_t tile);

/* True if a vehicle box centred on world pixel (px,py) overlaps solid ground. */
int cm_box_blocked(int px, int py);

/* Deterministic per-tile hash, used for roof colours and small details. */
uint32_t cm_hash(int tx, int ty);

/* Roof colour variant (0..3) of the city block a building tile belongs to. */
int cm_block_style(int tx, int ty);

/* 32-step cosine/sine lookup, Q8 fixed point (-256..256), used for the car's
 * heading. Index 0 points north; indices wrap modulo 32. */
extern const int16_t cm_cos_table[32];
extern const int16_t cm_sin_table[32];

#endif
