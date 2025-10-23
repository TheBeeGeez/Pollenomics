#ifndef HEX_H
#define HEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "params.h"
#include "tile_core.h"

typedef TileTerrain HexTerrain;

struct FlowerSystem;

typedef struct HexTile {
    HexTerrain terrain;
    float nectar_stock;
    float nectar_capacity;
    float nectar_recharge_rate;
    float nectar_recharge_multiplier;
    float flower_quality;
    float flower_viscosity;
    int16_t patch_id;
    float flow_capacity;
    uint16_t flower_archetype_id;
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
    float nectar_recharge_multiplier;
    float flower_quality;
    float flower_viscosity;
    float flow_capacity;
    uint16_t flower_archetype_id;
    const char *flower_archetype_name;
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
    TileRegistry tile_registry;
    struct FlowerSystem *flower_system;
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

bool hex_world_tile_from_world(const HexWorld *world, float world_x, float world_y, size_t *out_index);
bool hex_world_tile_is_floral(const HexWorld *world, size_t index);
float hex_world_tile_harvest(HexWorld *world, size_t index, float request_uL, float *quality_out);
void hex_world_tile_set_floral(HexWorld *world,
                               size_t index,
                               float capacity,
                               float stock,
                               float recharge_rate,
                               float quality,
                               float viscosity);
void hex_world_apply_palette(HexWorld *world, bool nectar_heatmap_enabled);

#endif  // HEX_H
