#include "hex.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"
#include <corecrt_math_defines.h>

static uint32_t make_color_rgba(float r, float g, float b, float a) {
    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    if (g < 0.0f) g = 0.0f;
    if (g > 1.0f) g = 1.0f;
    if (b < 0.0f) b = 0.0f;
    if (b > 1.0f) b = 1.0f;
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    uint32_t ri = (uint32_t)(r * 255.0f + 0.5f);
    uint32_t gi = (uint32_t)(g * 255.0f + 0.5f);
    uint32_t bi = (uint32_t)(b * 255.0f + 0.5f);
    uint32_t ai = (uint32_t)(a * 255.0f + 0.5f);
    if (ri > 255U) ri = 255U;
    if (gi > 255U) gi = 255U;
    if (bi > 255U) bi = 255U;
    if (ai > 255U) ai = 255U;
    return (ri << 24) | (gi << 16) | (bi << 8) | ai;
}

static uint32_t modulate_color(uint32_t rgba, float multiplier) {
    if (multiplier < 0.0f) {
        multiplier = 0.0f;
    }
    const float inv255 = 1.0f / 255.0f;
    float r = ((float)((rgba >> 24) & 0xFFu)) * inv255;
    float g = ((float)((rgba >> 16) & 0xFFu)) * inv255;
    float b = ((float)((rgba >> 8) & 0xFFu)) * inv255;
    float a = ((float)(rgba & 0xFFu)) * inv255;
    r *= multiplier;
    g *= multiplier;
    b *= multiplier;
    return make_color_rgba(r, g, b, a);
}

static void hex_world_setup_palette(HexWorld *world) {
    if (!world) {
        return;
    }
    world->palette[HEX_TERRAIN_OPEN] = make_color_rgba(0.80f, 0.82f, 0.86f, 0.55f);
    world->palette[HEX_TERRAIN_FOREST] = make_color_rgba(0.26f, 0.58f, 0.32f, 0.68f);
    world->palette[HEX_TERRAIN_MOUNTAIN] = make_color_rgba(0.55f, 0.46f, 0.36f, 0.68f);
    world->palette[HEX_TERRAIN_WATER] = make_color_rgba(0.28f, 0.50f, 0.82f, 0.62f);
    world->palette[HEX_TERRAIN_HIVE] = make_color_rgba(0.93f, 0.78f, 0.24f, 0.78f);
    world->palette[HEX_TERRAIN_FLOWERS] = make_color_rgba(0.90f, 0.42f, 0.72f, 0.72f);
    world->palette[HEX_TERRAIN_ENTRANCE] = make_color_rgba(0.20f, 0.85f, 0.82f, 0.80f);
}

static int terrain_pattern(int q, int r) {
    int n = (q * 92837111) ^ (r * 689287499);
    if (n < 0) {
        n = -n;
    }
    return n % 11;
}

static void assign_tile_properties(HexTile *tile, HexTerrain terrain) {
    if (!tile) {
        return;
    }
    tile->terrain = terrain;
    tile->nectar_stock = 0.0f;
    tile->nectar_capacity = 0.0f;
    tile->nectar_recharge_rate = 0.0f;
    tile->nectar_recharge_multiplier = 1.0f;
    tile->flower_quality = 0.0f;
    tile->flower_viscosity = 1.0f;
    tile->patch_id = -1;
    tile->flow_capacity = 10.0f;

    switch (terrain) {
        case HEX_TERRAIN_FOREST:
            tile->flow_capacity = 6.0f;
            break;
        case HEX_TERRAIN_MOUNTAIN:
            tile->flow_capacity = 3.0f;
            break;
        case HEX_TERRAIN_WATER:
            tile->flow_capacity = 1.0f;
            break;
        case HEX_TERRAIN_HIVE:
            tile->flow_capacity = 45.0f;
            break;
        case HEX_TERRAIN_FLOWERS:
            tile->nectar_capacity = 180.0f;
            tile->nectar_stock = 120.0f;
            tile->nectar_recharge_rate = 12.0f;
            tile->flower_quality = 0.75f;
            tile->flower_viscosity = 1.0f;
            tile->flow_capacity = 18.0f;
            break;
        case HEX_TERRAIN_ENTRANCE:
            tile->flow_capacity = 60.0f;
            break;
        case HEX_TERRAIN_OPEN:
        default:
            tile->flow_capacity = 12.0f;
            break;
    }
}

static bool hex_world_build(HexWorld *world, const Params *params) {
    if (!world || !params) {
        return false;
    }

    float radius = params->hex.cell_radius;
    if (radius <= 0.0f) {
        LOG_ERROR("hex: cell_radius must be > 0 (got %.3f)", radius);
        return false;
    }

    int q_min = params->hex.q_min;
    int q_max = params->hex.q_max;
    int r_min = params->hex.r_min;
    int r_max = params->hex.r_max;
    int width = (q_max - q_min) + 1;
    int height = (r_max - r_min) + 1;
    if (width <= 0 || height <= 0) {
        LOG_ERROR("hex: invalid axial bounds q[%d,%d] r[%d,%d]", q_min, q_max, r_min, r_max);
        return false;
    }

    size_t tile_count = (size_t)width * (size_t)height;
    if (tile_count == 0) {
        LOG_ERROR("hex: tile count is zero");
        return false;
    }

    size_t center_bytes = tile_count * 2 * sizeof(float);
    HexTile *tiles = (HexTile *)calloc(tile_count, sizeof(HexTile));
    float *centers = (float *)malloc(center_bytes);
    uint32_t *colors = (uint32_t *)malloc(tile_count * sizeof(uint32_t));
    if (!tiles || !centers || !colors) {
        free(tiles);
        free(centers);
        free(colors);
        LOG_ERROR("hex: allocation failed for %zu tiles", tile_count);
        return false;
    }

    memset(centers, 0, center_bytes);

    world->origin_x = params->hex.origin_x;
    world->origin_y = params->hex.origin_y;
    world->cell_radius = radius;
    world->sqrt3 = sqrtf(3.0f);
    world->inv_cell_radius = radius > 0.0f ? 1.0f / radius : 1.0f;
    world->q_min = q_min;
    world->q_max = q_max;
    world->r_min = r_min;
    world->r_max = r_max;
    world->width = width;
    world->height = height;
    world->tile_count = tile_count;
    world->tiles = tiles;
    world->centers_world_xy = centers;
    world->fill_rgba = colors;

    hex_world_setup_palette(world);

    bool hive_enabled = params->hive.rect_w > 0.0f && params->hive.rect_h > 0.0f;
    float hive_left = params->hive.rect_x;
    float hive_top = params->hive.rect_y;
    float hive_right = hive_left + params->hive.rect_w;
    float hive_bottom = hive_top + params->hive.rect_h;

    bool entrance_horizontal = params->hive.entrance_side == 0 || params->hive.entrance_side == 1;
    float entrance_axis = 0.0f;
    float entrance_min = 0.0f;
    float entrance_max = 0.0f;
    if (hive_enabled) {
        float half_gap = params->hive.entrance_width * 0.5f;
        if (entrance_horizontal) {
            float gap_center = hive_left + params->hive.entrance_t * params->hive.rect_w;
            entrance_axis = (params->hive.entrance_side == 0) ? hive_top : hive_bottom;
            entrance_min = fmaxf(hive_left, gap_center - half_gap);
            entrance_max = fminf(hive_right, gap_center + half_gap);
        } else {
            float gap_center = hive_top + params->hive.entrance_t * params->hive.rect_h;
            entrance_axis = (params->hive.entrance_side == 2) ? hive_left : hive_right;
            entrance_min = fmaxf(hive_top, gap_center - half_gap);
            entrance_max = fminf(hive_bottom, gap_center + half_gap);
        }
    }

    size_t index = 0;
    for (int r = r_min; r <= r_max; ++r) {
        for (int q = q_min; q <= q_max; ++q) {
            float cx = 0.0f;
            float cy = 0.0f;
            float fq = (float)q;
            float fr = (float)r;
            cx = world->origin_x + world->cell_radius * world->sqrt3 * (fq + fr * 0.5f);
            cy = world->origin_y + world->cell_radius * 1.5f * fr;
            centers[2 * index + 0] = cx;
            centers[2 * index + 1] = cy;

            HexTile *tile = &tiles[index];
            HexTerrain terrain = HEX_TERRAIN_OPEN;

            if (hive_enabled && cx >= hive_left && cx <= hive_right && cy >= hive_top && cy <= hive_bottom) {
                terrain = HEX_TERRAIN_HIVE;
            } else if (hive_enabled) {
                float axis_delta;
                float along_min = entrance_min - world->cell_radius;
                float along_max = entrance_max + world->cell_radius;
                if (entrance_horizontal) {
                    axis_delta = fabsf(cy - entrance_axis);
                    if (cx >= along_min && cx <= along_max && axis_delta <= world->cell_radius * 0.9f) {
                        terrain = HEX_TERRAIN_ENTRANCE;
                    }
                } else {
                    axis_delta = fabsf(cx - entrance_axis);
                    if (cy >= along_min && cy <= along_max && axis_delta <= world->cell_radius * 0.9f) {
                        terrain = HEX_TERRAIN_ENTRANCE;
                    }
                }
            }

            if (terrain == HEX_TERRAIN_OPEN) {
                int pattern = terrain_pattern(q, r);
                if (pattern <= 1) {
                    terrain = HEX_TERRAIN_FOREST;
                } else if (pattern == 2) {
                    terrain = HEX_TERRAIN_WATER;
                } else if (pattern == 3) {
                    terrain = HEX_TERRAIN_MOUNTAIN;
                } else if (pattern == 4 || pattern == 5 || pattern == 6) {
                    terrain = HEX_TERRAIN_FLOWERS;
                } else {
                    terrain = HEX_TERRAIN_OPEN;
                }
            }

            assign_tile_properties(tile, terrain);
            colors[index] = world->palette[tile->terrain];
            ++index;
        }
    }

    LOG_INFO("hex: built grid %d x %d (%zu tiles) radius=%.1f", width, height, tile_count, radius);
    return true;
}

bool hex_world_init(HexWorld *world, const Params *params) {
    if (!world) {
        return false;
    }
    HexWorld temp;
    memset(&temp, 0, sizeof(temp));
    if (!hex_world_build(&temp, params)) {
        hex_world_shutdown(&temp);
        return false;
    }
    *world = temp;
    return true;
}

bool hex_world_rebuild(HexWorld *world, const Params *params) {
    if (!world) {
        return false;
    }
    HexWorld temp;
    memset(&temp, 0, sizeof(temp));
    if (!hex_world_build(&temp, params)) {
        hex_world_shutdown(&temp);
        return false;
    }
    hex_world_shutdown(world);
    *world = temp;
    return true;
}

void hex_world_shutdown(HexWorld *world) {
    if (!world) {
        return;
    }
    free(world->tiles);
    free(world->centers_world_xy);
    free(world->fill_rgba);
    memset(world, 0, sizeof(*world));
}

float hex_world_cell_radius(const HexWorld *world) {
    return world ? world->cell_radius : 0.0f;
}

size_t hex_world_tile_count(const HexWorld *world) {
    return world ? world->tile_count : 0u;
}

const float *hex_world_centers_xy(const HexWorld *world) {
    return world ? world->centers_world_xy : NULL;
}

const uint32_t *hex_world_colors_rgba(const HexWorld *world) {
    return world ? world->fill_rgba : NULL;
}

bool hex_world_in_bounds(const HexWorld *world, int q, int r) {
    if (!world) {
        return false;
    }
    return q >= world->q_min && q <= world->q_max && r >= world->r_min && r <= world->r_max;
}

size_t hex_world_index(const HexWorld *world, int q, int r) {
    if (!world || !hex_world_in_bounds(world, q, r)) {
        return (size_t)SIZE_MAX;
    }
    size_t col = (size_t)(q - world->q_min);
    size_t row = (size_t)(r - world->r_min);
    return row * (size_t)world->width + col;
}

bool hex_world_index_to_axial(const HexWorld *world, size_t index, int *out_q, int *out_r) {
    if (!world || index >= world->tile_count) {
        return false;
    }
    size_t row = index / (size_t)world->width;
    size_t col = index % (size_t)world->width;
    if (out_q) {
        *out_q = world->q_min + (int)col;
    }
    if (out_r) {
        *out_r = world->r_min + (int)row;
    }
    return true;
}

bool hex_world_tile_debug_info(const HexWorld *world, size_t index, HexTileDebugInfo *out_info) {
    if (!world || !out_info || index >= world->tile_count) {
        return false;
    }
    int q = 0;
    int r = 0;
    if (!hex_world_index_to_axial(world, index, &q, &r)) {
        return false;
    }
    out_info->q = q;
    out_info->r = r;
    out_info->center_x = world->centers_world_xy[2 * index + 0];
    out_info->center_y = world->centers_world_xy[2 * index + 1];
    const HexTile *tile = &world->tiles[index];
    out_info->terrain = tile->terrain;
    out_info->nectar_stock = tile->nectar_stock;
    out_info->nectar_capacity = tile->nectar_capacity;
    out_info->nectar_recharge_rate = tile->nectar_recharge_rate;
    out_info->nectar_recharge_multiplier = tile->nectar_recharge_multiplier;
    out_info->flower_quality = tile->flower_quality;
    out_info->flower_viscosity = tile->flower_viscosity;
    out_info->flow_capacity = tile->flow_capacity;
    return true;
}

void hex_world_axial_to_world(const HexWorld *world, int q, int r, float *out_x, float *out_y) {
    if (!world) {
        return;
    }
    float fq = (float)q;
    float fr = (float)r;
    float cx = world->origin_x + world->cell_radius * world->sqrt3 * (fq + fr * 0.5f);
    float cy = world->origin_y + world->cell_radius * 1.5f * fr;
    if (out_x) {
        *out_x = cx;
    }
    if (out_y) {
        *out_y = cy;
    }
}

void hex_world_world_to_axial(const HexWorld *world, float world_x, float world_y, float *out_q,
                              float *out_r) {
    if (!world) {
        return;
    }
    float dx = world_x - world->origin_x;
    float dy = world_y - world->origin_y;
    float q = (world->sqrt3 / 3.0f * dx - dy / 3.0f) * world->inv_cell_radius;
    float r = (2.0f / 3.0f * dy) * world->inv_cell_radius;
    if (out_q) {
        *out_q = q;
    }
    if (out_r) {
        *out_r = r;
    }
}

void hex_world_axial_round(float qf, float rf, int *out_q, int *out_r) {
    float sf = -qf - rf;
    int rq = (int)roundf(qf);
    int rr = (int)roundf(rf);
    int rs = (int)roundf(sf);

    float q_diff = fabsf((float)rq - qf);
    float r_diff = fabsf((float)rr - rf);
    float s_diff = fabsf((float)rs - sf);

    if (q_diff > r_diff && q_diff > s_diff) {
        rq = -rr - rs;
    } else if (r_diff > s_diff) {
        rr = -rq - rs;
    } else {
        rs = -rq - rr;
    }

    if (out_q) {
        *out_q = rq;
    }
    if (out_r) {
        *out_r = rr;
    }
}

bool hex_world_pick(const HexWorld *world, float world_x, float world_y, int *out_q, int *out_r) {
    if (!world) {
        return false;
    }
    float qf = 0.0f;
    float rf = 0.0f;
    hex_world_world_to_axial(world, world_x, world_y, &qf, &rf);
    int q = 0;
    int r = 0;
    hex_world_axial_round(qf, rf, &q, &r);
    if (!hex_world_in_bounds(world, q, r)) {
        return false;
    }
    if (out_q) {
        *out_q = q;
    }
    if (out_r) {
        *out_r = r;
    }
    return true;
}

void hex_world_tile_corners(const HexWorld *world, int q, int r, float (*out_xy)[2]) {
    if (!world || !out_xy) {
        return;
    }
    float cx = 0.0f;
    float cy = 0.0f;
    hex_world_axial_to_world(world, q, r, &cx, &cy);
    const float radius = world->cell_radius;
    for (int i = 0; i < 6; ++i) {
        float angle_deg = 60.0f * (float)i - 30.0f;
        float angle_rad = angle_deg * (float)M_PI / 180.0f;
        float px = cx + radius * cosf(angle_rad);
        float py = cy + radius * sinf(angle_rad);
        out_xy[i][0] = px;
        out_xy[i][1] = py;
    }
}

bool hex_world_tile_from_world(const HexWorld *world, float world_x, float world_y, size_t *out_index) {
    if (!world || !out_index) {
        return false;
    }
    int q = 0;
    int r = 0;
    if (!hex_world_pick(world, world_x, world_y, &q, &r)) {
        return false;
    }
    size_t index = hex_world_index(world, q, r);
    if (index == (size_t)SIZE_MAX) {
        return false;
    }
    *out_index = index;
    return true;
}

bool hex_world_tile_is_floral(const HexWorld *world, size_t index) {
    if (!world || index >= world->tile_count) {
        return false;
    }
    const HexTile *tile = &world->tiles[index];
    return tile->terrain == HEX_TERRAIN_FLOWERS && tile->nectar_capacity > 0.0f;
}

float hex_world_tile_harvest(HexWorld *world, size_t index, float request_uL, float *quality_out) {
    if (!world || index >= world->tile_count || request_uL <= 0.0f) {
        if (quality_out) {
            *quality_out = 0.0f;
        }
        return 0.0f;
    }
    HexTile *tile = &world->tiles[index];
    if (!hex_world_tile_is_floral(world, index)) {
        if (quality_out) {
            *quality_out = 0.0f;
        }
        return 0.0f;
    }

    float viscosity = tile->flower_viscosity;
    if (viscosity <= 0.0f) {
        viscosity = 1.0f;
    }
    float viscosity_scale = 1.0f / sqrtf(viscosity);
    if (viscosity_scale < 0.05f) {
        viscosity_scale = 0.05f;
    }

    float harvest = request_uL * viscosity_scale;
    if (harvest > tile->nectar_stock) {
        harvest = tile->nectar_stock;
    }
    if (harvest <= 0.0f) {
        if (quality_out) {
            *quality_out = tile->flower_quality;
        }
        return 0.0f;
    }

    tile->nectar_stock -= harvest;
    if (tile->nectar_stock < 0.0f) {
        tile->nectar_stock = 0.0f;
    }
    if (quality_out) {
        *quality_out = tile->flower_quality;
    }
    return harvest;
}

void hex_world_tile_set_floral(HexWorld *world,
                               size_t index,
                               float capacity,
                               float stock,
                               float recharge_rate,
                               float quality,
                               float viscosity) {
    if (!world || index >= world->tile_count) {
        return;
    }
    HexTile *tile = &world->tiles[index];
    tile->terrain = HEX_TERRAIN_FLOWERS;
    tile->nectar_capacity = capacity >= 0.0f ? capacity : 0.0f;
    if (stock < 0.0f) {
        stock = 0.0f;
    }
    if (stock > tile->nectar_capacity && tile->nectar_capacity > 0.0f) {
        stock = tile->nectar_capacity;
    }
    tile->nectar_stock = stock;
    tile->nectar_recharge_rate = recharge_rate >= 0.0f ? recharge_rate : 0.0f;
    if (quality < 0.0f) quality = 0.0f;
    if (quality > 1.0f) quality = 1.0f;
    tile->flower_quality = quality;
    if (viscosity <= 0.0f) {
        viscosity = 1.0f;
    }
    tile->flower_viscosity = viscosity;
    tile->nectar_recharge_multiplier = 1.0f;
    tile->patch_id = -1;
}

void hex_world_apply_palette(HexWorld *world, bool nectar_heatmap_enabled) {
    if (!world || !world->fill_rgba) {
        return;
    }
    for (size_t i = 0; i < world->tile_count; ++i) {
        const HexTile *tile = &world->tiles[i];
        uint32_t base = world->palette[tile->terrain];
        if (nectar_heatmap_enabled && tile->terrain == HEX_TERRAIN_FLOWERS && tile->nectar_capacity > 0.0f) {
            float ratio = tile->nectar_stock / tile->nectar_capacity;
            if (ratio < 0.0f) {
                ratio = 0.0f;
            }
            if (ratio > 1.0f) {
                ratio = 1.0f;
            }
            float brightness = 0.25f + 0.75f * ratio;
            world->fill_rgba[i] = modulate_color(base, brightness);
        } else {
            world->fill_rgba[i] = base;
        }
    }
}

