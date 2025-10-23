#include "hex.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"
#include "world/tiles/tile_flower.h"
#include <corecrt_math_defines.h>

typedef struct HiveStorageTilePayload {
    size_t tile_index;
    float stock_uL;
    float capacity_uL;
} HiveStorageTilePayload;

typedef struct HiveEntranceCandidate {
    size_t tile_index;
    float dot_score;
} HiveEntranceCandidate;

typedef struct HiveSystem {
    bool enabled;
    float center_x;
    float center_y;
    int center_q;
    int center_r;
    int radius_tiles;
    int storage_radius_tiles;
    float honey_total_uL;
    float pollen_total_uL;
    HiveStorageTilePayload *storage_tiles;
    size_t storage_tile_count;
    size_t *entrance_tile_indices;
    size_t entrance_tile_count;
} HiveSystem;

static void hive_system_free(HiveSystem *hive) {
    if (!hive) {
        return;
    }
    free(hive->storage_tiles);
    free(hive->entrance_tile_indices);
    free(hive);
}

static const int HIVE_DIR_Q[6] = {0, 1, 1, 0, -1, -1};
static const int HIVE_DIR_R[6] = {-1, -1, 0, 1, 1, 0};

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

static void hex_world_setup_palette(HexWorld *world) {
    if (!world) {
        return;
    }
    world->palette[HEX_TERRAIN_OPEN] = make_color_rgba(0.80f, 0.82f, 0.86f, 0.55f);
    world->palette[HEX_TERRAIN_FOREST] = make_color_rgba(0.26f, 0.58f, 0.32f, 0.68f);
    world->palette[HEX_TERRAIN_MOUNTAIN] = make_color_rgba(0.55f, 0.46f, 0.36f, 0.68f);
    world->palette[HEX_TERRAIN_WATER] = make_color_rgba(0.28f, 0.50f, 0.82f, 0.62f);
    world->palette[HEX_TERRAIN_HIVE_INTERIOR] = make_color_rgba(0.93f, 0.85f, 0.58f, 0.78f);
    world->palette[HEX_TERRAIN_HIVE_STORAGE] = make_color_rgba(0.98f, 0.74f, 0.18f, 0.82f);
    world->palette[HEX_TERRAIN_HIVE_WALL] = make_color_rgba(0.36f, 0.22f, 0.10f, 0.92f);
    world->palette[HEX_TERRAIN_HIVE_ENTRANCE] = make_color_rgba(0.18f, 0.78f, 0.82f, 0.88f);
    world->palette[HEX_TERRAIN_FLOWERS] = make_color_rgba(0.90f, 0.42f, 0.72f, 0.72f);
}

static int terrain_pattern(int q, int r) {
    int n = (q * 92837111) ^ (r * 689287499);
    if (n < 0) {
        n = -n;
    }
    return n % 11;
}

static int hex_distance_axial(int q1, int r1, int q2, int r2) {
    int dq = q1 - q2;
    int dr = r1 - r2;
    int ds = -dq - dr;
    if (dq < 0) dq = -dq;
    if (dr < 0) dr = -dr;
    if (ds < 0) ds = -ds;
    int max = dq;
    if (dr > max) max = dr;
    if (ds > max) max = ds;
    return max;
}

static bool push_size_t(size_t **data, size_t *count, size_t *capacity, size_t value) {
    if (!data || !count || !capacity) {
        return false;
    }
    if (*count >= *capacity) {
        size_t new_cap = (*capacity == 0) ? 16 : (*capacity * 2);
        size_t *new_data = (size_t *)realloc(*data, new_cap * sizeof(size_t));
        if (!new_data) {
            return false;
        }
        *data = new_data;
        *capacity = new_cap;
    }
    (*data)[*count] = value;
    (*count)++;
    return true;
}

static bool push_candidate(HiveEntranceCandidate **data,
                           size_t *count,
                           size_t *capacity,
                           const HiveEntranceCandidate *value) {
    if (!data || !count || !capacity || !value) {
        return false;
    }
    if (*count >= *capacity) {
        size_t new_cap = (*capacity == 0) ? 16 : (*capacity * 2);
        HiveEntranceCandidate *new_data =
            (HiveEntranceCandidate *)realloc(*data, new_cap * sizeof(HiveEntranceCandidate));
        if (!new_data) {
            return false;
        }
        *data = new_data;
        *capacity = new_cap;
    }
    (*data)[*count] = *value;
    (*count)++;
    return true;
}

static int compare_candidate_desc(const void *lhs, const void *rhs) {
    const HiveEntranceCandidate *a = (const HiveEntranceCandidate *)lhs;
    const HiveEntranceCandidate *b = (const HiveEntranceCandidate *)rhs;
    if (b->dot_score > a->dot_score) {
        return 1;
    }
    if (b->dot_score < a->dot_score) {
        return -1;
    }
    if (b->tile_index > a->tile_index) {
        return 1;
    }
    if (b->tile_index < a->tile_index) {
        return -1;
    }
    return 0;
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
    tile->flower_archetype_id = 0;
    tile->hive_honey_stock = 0.0f;
    tile->hive_honey_capacity = 0.0f;
    tile->base_cost = 1.0f;
    tile->passable = true;
    tile->hive_deposit_enabled = false;
    tile->hive_storage_slot = -1;

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
        case HEX_TERRAIN_HIVE_INTERIOR:
            tile->flow_capacity = 42.0f;
            tile->base_cost = 1.0f;
            break;
        case HEX_TERRAIN_HIVE_STORAGE:
            tile->flow_capacity = 40.0f;
            tile->base_cost = 1.0f;
            tile->hive_deposit_enabled = true;
            break;
        case HEX_TERRAIN_HIVE_WALL:
            tile->flow_capacity = 0.0f;
            tile->base_cost = 1e6f;
            tile->passable = false;
            break;
        case HEX_TERRAIN_HIVE_ENTRANCE:
            tile->flow_capacity = 60.0f;
            tile->base_cost = 0.7f;
            break;
        case HEX_TERRAIN_FLOWERS:
            tile->nectar_capacity = 180.0f;
            tile->nectar_stock = 120.0f;
            tile->nectar_recharge_rate = 12.0f;
            tile->flower_quality = 0.75f;
            tile->flower_viscosity = 1.0f;
            tile->flow_capacity = 18.0f;
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

    if (!world->flower_system) {
        world->flower_system = (FlowerSystem *)malloc(sizeof(FlowerSystem));
        if (!world->flower_system) {
            free(tiles);
            free(centers);
            free(colors);
            LOG_ERROR("hex: failed to allocate flower system");
            return false;
        }
        tile_flower_system_init(world->flower_system);
    }

    tile_registry_init(&world->tile_registry);
    tile_flower_register(&world->tile_registry, world->flower_system);

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

    const TileTypeRegistration *flower_entry =
        tile_registry_get(&world->tile_registry, HEX_TERRAIN_FLOWERS);
    if (flower_entry && flower_entry->vtable && flower_entry->vtable->on_world_reset) {
        flower_entry->vtable->on_world_reset(flower_entry->user_data, world, tile_count);
    }

    HiveSystem *hive = NULL;
    HiveEntranceCandidate *entrance_candidates = NULL;
    size_t entrance_candidate_count = 0;
    size_t entrance_candidate_cap = 0;
    size_t *storage_indices = NULL;
    size_t storage_index_count = 0;
    size_t storage_index_cap = 0;

    bool hive_enabled = params->hive.radius_tiles > 0;
    int hive_radius = params->hive.radius_tiles;
    if (hive_radius < 0) {
        hive_radius = 0;
        hive_enabled = false;
    }
    int storage_radius = params->hive.storage_radius_tiles;
    if (storage_radius < 0) {
        storage_radius = 0;
    }
    if (hive_radius <= 0) {
        storage_radius = 0;
    } else if (storage_radius > hive_radius - 1) {
        storage_radius = hive_radius - 1;
        if (storage_radius < 0) {
            storage_radius = 0;
        }
    }

    int entrance_dir = params->hive.entrance_dir;
    if (entrance_dir < 0 || entrance_dir > 5) {
        entrance_dir = 3;
    }
    int entrance_width = params->hive.entrance_width_tiles;
    if (entrance_width <= 0) {
        entrance_width = 1;
    }

    float hive_center_x = params->hive.center_x;
    float hive_center_y = params->hive.center_y;
    float entrance_target_x = hive_center_x;
    float entrance_target_y = hive_center_y;
    float entrance_target_vx = 0.0f;
    float entrance_target_vy = 0.0f;
    float entrance_target_len = 0.0f;
    int center_q = 0;
    int center_r = 0;
    int entrance_center_q = 0;
    int entrance_center_r = 0;

    if (hive_enabled && hive_radius > 0) {
        hive = (HiveSystem *)calloc(1, sizeof(HiveSystem));
        if (!hive) {
            free(tiles);
            free(centers);
            free(colors);
            LOG_ERROR("hex: failed to allocate hive system");
            return false;
        }
        hive->enabled = true;
        hive->center_x = hive_center_x;
        hive->center_y = hive_center_y;
        hive->radius_tiles = hive_radius;
        hive->storage_radius_tiles = storage_radius;
        hive->honey_total_uL = 0.0f;
        hive->pollen_total_uL = 0.0f;
        float qf = 0.0f;
        float rf = 0.0f;
        hex_world_world_to_axial(world, hive_center_x, hive_center_y, &qf, &rf);
        hex_world_axial_round(qf, rf, &center_q, &center_r);
        hive->center_q = center_q;
        hive->center_r = center_r;
        int eq = center_q + HIVE_DIR_Q[entrance_dir] * hive_radius;
        int er = center_r + HIVE_DIR_R[entrance_dir] * hive_radius;
        entrance_center_q = eq;
        entrance_center_r = er;
        hex_world_axial_to_world(world, eq, er, &entrance_target_x, &entrance_target_y);
        entrance_target_vx = entrance_target_x - hive_center_x;
        entrance_target_vy = entrance_target_y - hive_center_y;
        entrance_target_len = sqrtf(entrance_target_vx * entrance_target_vx +
                                    entrance_target_vy * entrance_target_vy);
    } else {
        hive_enabled = false;
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

            if (hive_enabled) {
                int dist = hex_distance_axial(q, r, center_q, center_r);
                if (dist < hive_radius) {
                    if (storage_radius > 0 && dist <= storage_radius) {
                        terrain = HEX_TERRAIN_HIVE_STORAGE;
                    } else {
                        terrain = HEX_TERRAIN_HIVE_INTERIOR;
                    }
                } else if (dist == hive_radius) {
                    terrain = HEX_TERRAIN_HIVE_WALL;
                    if (entrance_target_len > 1e-5f) {
                        float vx = cx - hive_center_x;
                        float vy = cy - hive_center_y;
                        float vlen = sqrtf(vx * vx + vy * vy);
                        float dot = 0.0f;
                        if (vlen > 1e-5f) {
                            dot = (vx * entrance_target_vx + vy * entrance_target_vy) /
                                  (vlen * entrance_target_len);
                        }
                        int dist_to_target = hex_distance_axial(q, r, entrance_center_q, entrance_center_r);
                        float score = -(float)dist_to_target + dot * 0.001f;
                        HiveEntranceCandidate candidate = {index, score};
                        if (!push_candidate(&entrance_candidates,
                                            &entrance_candidate_count,
                                            &entrance_candidate_cap,
                                            &candidate)) {
                            LOG_ERROR("hex: failed to allocate hive entrance candidate");
                            free(storage_indices);
                            free(entrance_candidates);
                            hive_system_free(hive);
                            free(tiles);
                            free(centers);
                            free(colors);
                            return false;
                        }
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
            if (terrain == HEX_TERRAIN_HIVE_STORAGE) {
                tile->hive_honey_capacity = 900.0f;
                size_t slot_index = storage_index_count;
                if (!push_size_t(&storage_indices, &storage_index_count, &storage_index_cap, index)) {
                    LOG_ERROR("hex: failed to allocate hive storage list");
                    free(storage_indices);
                    free(entrance_candidates);
                    hive_system_free(hive);
                    free(tiles);
                    free(centers);
                    free(colors);
                    return false;
                }
                tile->hive_storage_slot = (int16_t)slot_index;
            }
            if (terrain == HEX_TERRAIN_FLOWERS && flower_entry && flower_entry->vtable &&
                flower_entry->vtable->generate_tile) {
                uint64_t seed = ((uint64_t)(uint32_t)q << 32) ^ (uint64_t)(uint32_t)r;
                flower_entry->vtable->generate_tile(flower_entry->user_data, world, index, q, r, seed);
            }
            uint32_t base_color = world->palette[tile->terrain];
            if (tile->terrain == HEX_TERRAIN_FLOWERS && world->flower_system) {
                base_color = tile_flower_color(world->flower_system, index, base_color);
            }
            colors[index] = base_color;
            ++index;
        }
    }

    if (hive) {
        if (entrance_candidate_count > 0 && entrance_width > 0) {
            if ((size_t)entrance_width > entrance_candidate_count) {
                entrance_width = (int)entrance_candidate_count;
            }
            if (entrance_width <= 0) {
                entrance_width = 1;
            }
            qsort(entrance_candidates,
                  entrance_candidate_count,
                  sizeof(HiveEntranceCandidate),
                  compare_candidate_desc);
            hive->entrance_tile_indices =
                (size_t *)calloc((size_t)entrance_width, sizeof(size_t));
            if (!hive->entrance_tile_indices) {
                LOG_ERROR("hex: failed to allocate hive entrance list");
                free(storage_indices);
                free(entrance_candidates);
                hive_system_free(hive);
                free(tiles);
                free(centers);
                free(colors);
                return false;
            }
            hive->entrance_tile_count = (size_t)entrance_width;
            for (int i = 0; i < entrance_width; ++i) {
                size_t idx = entrance_candidates[i].tile_index;
                if (idx >= tile_count) {
                    continue;
                }
                hive->entrance_tile_indices[i] = idx;
                HexTile *tile = &tiles[idx];
                assign_tile_properties(tile, HEX_TERRAIN_HIVE_ENTRANCE);
                uint32_t base_color = world->palette[tile->terrain];
                colors[idx] = base_color;
            }
        }
        if (storage_index_count > 0) {
            hive->storage_tiles =
                (HiveStorageTilePayload *)calloc(storage_index_count, sizeof(HiveStorageTilePayload));
            if (!hive->storage_tiles) {
                LOG_ERROR("hex: failed to allocate hive storage payloads");
                free(storage_indices);
                free(entrance_candidates);
                hive_system_free(hive);
                free(tiles);
                free(centers);
                free(colors);
                return false;
            }
            hive->storage_tile_count = storage_index_count;
            for (size_t i = 0; i < storage_index_count; ++i) {
                size_t idx = storage_indices[i];
                if (idx >= tile_count) {
                    continue;
                }
                HexTile *tile = &tiles[idx];
                tile->hive_storage_slot = (int16_t)i;
                hive->storage_tiles[i].tile_index = idx;
                hive->storage_tiles[i].capacity_uL = tile->hive_honey_capacity;
                hive->storage_tiles[i].stock_uL = tile->hive_honey_stock;
            }
        }
        world->hive_system = hive;
    } else {
        world->hive_system = NULL;
        hive_system_free(hive);
    }

    free(storage_indices);
    free(entrance_candidates);

    hex_world_apply_palette(world, false);

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
    if (world->flower_system) {
        tile_flower_system_shutdown(world->flower_system);
        free(world->flower_system);
        world->flower_system = NULL;
    }
    hive_system_free(world->hive_system);
    world->hive_system = NULL;
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
    out_info->flower_archetype_id = tile->flower_archetype_id;
    out_info->flower_archetype_name = NULL;
    out_info->hive_honey_stock = tile->hive_honey_stock;
    out_info->hive_honey_capacity = tile->hive_honey_capacity;
    out_info->hive_base_cost = tile->base_cost;
    out_info->hive_passable = tile->passable;
    out_info->hive_allows_deposit = tile->hive_deposit_enabled;
    out_info->hive_total_honey = world->hive_system ? world->hive_system->honey_total_uL : 0.0f;
    out_info->hive_total_pollen = world->hive_system ? world->hive_system->pollen_total_uL : 0.0f;
    if (tile->terrain == HEX_TERRAIN_FLOWERS && world->flower_system) {
        out_info->flower_archetype_name = tile_flower_archetype_name(world->flower_system, index);
    }
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
    const TileTypeRegistration *entry = tile_registry_get(&world->tile_registry, tile->terrain);
    if (entry && entry->vtable && entry->vtable->is_floral) {
        return entry->vtable->is_floral(entry->user_data, index);
    }
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

    float effective_request = request_uL * viscosity_scale;

    const TileTypeRegistration *entry = tile_registry_get(&world->tile_registry, tile->terrain);
    if (entry && entry->vtable && entry->vtable->harvest) {
        float harvested = entry->vtable->harvest(entry->user_data, world, index, effective_request, quality_out);
        tile = &world->tiles[index];
        if (quality_out && *quality_out <= 0.0f) {
            *quality_out = tile->flower_quality;
        }
        return harvested;
    }

    float harvest = effective_request;
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
    if (world->flower_system) {
        tile_flower_override_payload(world->flower_system,
                                     world,
                                     index,
                                     tile->nectar_capacity,
                                     tile->nectar_stock,
                                     tile->nectar_recharge_rate,
                                     tile->nectar_recharge_multiplier,
                                     tile->flower_quality,
                                     tile->flower_viscosity);
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
    if (world->flower_system) {
        tile_flower_override_payload(world->flower_system,
                                     world,
                                     index,
                                     tile->nectar_capacity,
                                     tile->nectar_stock,
                                     tile->nectar_recharge_rate,
                                     tile->nectar_recharge_multiplier,
                                     tile->flower_quality,
                                     tile->flower_viscosity);
    }
}

void hex_world_apply_palette(HexWorld *world, bool nectar_heatmap_enabled) {
    if (!world || !world->fill_rgba) {
        return;
    }
    for (size_t i = 0; i < world->tile_count; ++i) {
        const HexTile *tile = &world->tiles[i];
        world->fill_rgba[i] = world->palette[tile->terrain];
    }
    for (int terrain = 0; terrain < HEX_TERRAIN_COUNT; ++terrain) {
        const TileTypeRegistration *entry = tile_registry_get(&world->tile_registry, (HexTerrain)terrain);
        if (entry && entry->vtable && entry->vtable->apply_palette) {
            entry->vtable->apply_palette(entry->user_data, world, nectar_heatmap_enabled);
        }
    }
}

bool hex_world_tile_passable(const HexWorld *world, size_t index) {
    if (!world || index >= world->tile_count || !world->tiles) {
        return true;
    }
    return world->tiles[index].passable;
}

bool hex_world_tile_allows_deposit(const HexWorld *world, size_t index) {
    if (!world || index >= world->tile_count || !world->tiles) {
        return false;
    }
    return world->tiles[index].hive_deposit_enabled;
}

static HiveStorageTilePayload *hive_lookup_storage(HexWorld *world, int16_t slot) {
    if (!world || !world->hive_system || !world->hive_system->storage_tiles || slot < 0) {
        return NULL;
    }
    size_t idx = (size_t)slot;
    if (idx >= world->hive_system->storage_tile_count) {
        return NULL;
    }
    return &world->hive_system->storage_tiles[idx];
}

float hex_world_hive_deposit_at_tile(HexWorld *world, size_t index, float request_uL) {
    if (!world || !world->hive_system || !world->hive_system->enabled || request_uL <= 0.0f) {
        return 0.0f;
    }
    if (!world->tiles || index >= world->tile_count) {
        return 0.0f;
    }
    HexTile *tile = &world->tiles[index];
    if (!tile->hive_deposit_enabled) {
        return 0.0f;
    }
    HiveStorageTilePayload *payload = hive_lookup_storage(world, tile->hive_storage_slot);
    if (!payload) {
        return 0.0f;
    }
    float capacity = payload->capacity_uL > 0.0f ? payload->capacity_uL : tile->hive_honey_capacity;
    float stock = payload->stock_uL;
    float space = capacity - stock;
    if (space <= 1e-6f) {
        return 0.0f;
    }
    float accepted = request_uL;
    if (accepted > space) {
        accepted = space;
    }
    if (accepted <= 0.0f) {
        return 0.0f;
    }
    payload->stock_uL += accepted;
    tile->hive_honey_stock = payload->stock_uL;
    world->hive_system->honey_total_uL += accepted;
    return accepted;
}

float hex_world_hive_deposit_world(HexWorld *world, float world_x, float world_y, float request_uL) {
    if (!world || !world->hive_system || !world->hive_system->enabled || request_uL <= 0.0f) {
        return 0.0f;
    }
    float remaining = request_uL;
    float deposited = 0.0f;
    size_t primary_index = (size_t)SIZE_MAX;
    if (hex_world_tile_from_world(world, world_x, world_y, &primary_index)) {
        float accepted = hex_world_hive_deposit_at_tile(world, primary_index, remaining);
        deposited += accepted;
        remaining -= accepted;
    }
    if (remaining <= 0.0f) {
        return deposited;
    }
    HiveSystem *hive = world->hive_system;
    if (!hive || !hive->storage_tiles) {
        return deposited;
    }
    for (size_t i = 0; i < hive->storage_tile_count && remaining > 0.0f; ++i) {
        size_t tile_index = hive->storage_tiles[i].tile_index;
        if (tile_index == primary_index) {
            continue;
        }
        float accepted = hex_world_hive_deposit_at_tile(world, tile_index, remaining);
        if (accepted > 0.0f) {
            deposited += accepted;
            remaining -= accepted;
        }
    }
    return deposited;
}

float hex_world_hive_total_honey(const HexWorld *world) {
    if (!world || !world->hive_system || !world->hive_system->enabled) {
        return 0.0f;
    }
    return world->hive_system->honey_total_uL;
}

float hex_world_hive_total_pollen(const HexWorld *world) {
    if (!world || !world->hive_system || !world->hive_system->enabled) {
        return 0.0f;
    }
    return world->hive_system->pollen_total_uL;
}

bool hex_world_hive_center(const HexWorld *world, float *out_x, float *out_y) {
    if (!world || !world->hive_system || !world->hive_system->enabled) {
        return false;
    }
    if (out_x) {
        *out_x = world->hive_system->center_x;
    }
    if (out_y) {
        *out_y = world->hive_system->center_y;
    }
    return true;
}

bool hex_world_hive_preferred_unload(const HexWorld *world, float *out_x, float *out_y) {
    if (!world || !world->hive_system || !world->hive_system->enabled) {
        return false;
    }
    if (world->hive_system->storage_tile_count > 0 && world->centers_world_xy) {
        size_t tile_index = world->hive_system->storage_tiles[0].tile_index;
        if (tile_index < world->tile_count) {
            if (out_x) {
                *out_x = world->centers_world_xy[2 * tile_index + 0];
            }
            if (out_y) {
                *out_y = world->centers_world_xy[2 * tile_index + 1];
            }
            return true;
        }
    }
    return hex_world_hive_center(world, out_x, out_y);
}

bool hex_world_hive_preferred_entrance(const HexWorld *world, float *out_x, float *out_y) {
    if (!world || !world->hive_system || !world->hive_system->enabled) {
        return false;
    }
    if (world->hive_system->entrance_tile_count > 0 && world->centers_world_xy) {
        size_t tile_index = world->hive_system->entrance_tile_indices[0];
        if (tile_index < world->tile_count) {
            if (out_x) {
                *out_x = world->centers_world_xy[2 * tile_index + 0];
            }
            if (out_y) {
                *out_y = world->centers_world_xy[2 * tile_index + 1];
            }
            return true;
        }
    }
    return hex_world_hive_center(world, out_x, out_y);
}

