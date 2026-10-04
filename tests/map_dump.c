/* Writes the whole city as a PPM, four pixels per tile, for the overview
 * picture made by scripts/render_screens.py: map_dump > map.ppm */
#include "../citymap.h"
#include <stdio.h>

int main(void) {
    static const unsigned char colour[CM_TILE_COUNT][3] = {
        {24, 96, 200}, {78, 78, 86}, {78, 78, 86}, {206, 150, 110}, {228, 226, 218},
        {60, 170, 70}, {255, 150, 0}, {255, 0, 255}, {232, 222, 190}, {150, 40, 40},
    };
    printf("P6\n%d %d\n255\n", CM_WORLD_COLS * 4, CM_WORLD_ROWS * 4);
    for (int y = 0; y < CM_WORLD_ROWS * 4; ++y)
        for (int x = 0; x < CM_WORLD_COLS * 4; ++x)
            fwrite(colour[cm_tile_at(x / 4, y / 4)], 1, 3, stdout);
    for (int i = 0; i < CM_POI_COUNT; ++i)
        fprintf(stderr, "%d %d %d %d\n", cm_pois[i].x, cm_pois[i].y, cm_pois[i].w, cm_pois[i].h);
    return 0;
}
