#ifndef HEX_H
#define HEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "params.h"

typedef enum HexTerrain {
    HEX_TERRAIN_OPEN = 0,
    HEX_TERRAIN_FOREST = 1,
    HEX_TERRAIN_MOUNTAIN = 2,
    HEX_TERRAIN_WATER = 3,
    HEX_TERRAIN_HIVE = 4,
    HEX_TERRAIN_FLOWERS = 5,
    HEX_TERRAIN_ENTRANCE = 6,
    HEX_TERRAIN_COUNT
} HexTerrain;

typedef struct HexTile {
    HexTerrain terrain;
    float nectar_stock;
    float nectar_capacity;
    float nectar_recharge_rate;
    float flow_capacity;
} HexTile;

typedef struct HexTileDebugInfo {
    int q;
    int r;
    float center_x;
    float center_y;
    HexTerrain terrain;
    float nectar_stock;
    float nectar_capacity;
    float nectar_recharge_rate;
    float flow_capacity;
} HexTileDebugInfo;

typedef struct HexWorld {
    float origin_x;
    float origin_y;
    float cell_radius;
    float sqrt3;
    float inv_cell_radius;
    int q_min;
    int q_max;
    int r_min;
    int r_max;
    int width;
    int height;
    size_t tile_count;
    HexTile *tiles;
    float *centers_world_xy;
    uint32_t *fill_rgba;
    uint32_t palette[HEX_TERRAIN_COUNT];
} HexWorld;

bool hex_world_init(HexWorld *world, const Params *params);
bool hex_world_rebuild(HexWorld *world, const Params *params);
void hex_world_shutdown(HexWorld *world);

float hex_world_cell_radius(const HexWorld *world);
size_t hex_world_tile_count(const HexWorld *world);
const float *hex_world_centers_xy(const HexWorld *world);
const uint32_t *hex_world_colors_rgba(const HexWorld *world);

bool hex_world_in_bounds(const HexWorld *world, int q, int r);
size_t hex_world_index(const HexWorld *world, int q, int r);
bool hex_world_index_to_axial(const HexWorld *world, size_t index, int *out_q, int *out_r);
bool hex_world_tile_debug_info(const HexWorld *world, size_t index, HexTileDebugInfo *out_info);

void hex_world_axial_to_world(const HexWorld *world, int q, int r, float *out_x, float *out_y);
void hex_world_world_to_axial(const HexWorld *world, float world_x, float world_y, float *out_q,
                              float *out_r);
void hex_world_axial_round(float qf, float rf, int *out_q, int *out_r);
bool hex_world_pick(const HexWorld *world, float world_x, float world_y, int *out_q, int *out_r);
void hex_world_tile_corners(const HexWorld *world, int q, int r, float (*out_xy)[2]);

#endif  // HEX_H
