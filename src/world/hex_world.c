#include "hex.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

static size_t hex_tile_count_from_bounds(int16_t q_min, int16_t q_max, int16_t r_min, int16_t r_max, uint16_t *out_width, uint16_t *out_height) {
    if (q_max < q_min || r_max < r_min) {
        if (out_width) {
            *out_width = 0;
        }
        if (out_height) {
            *out_height = 0;
        }
        return 0;
    }
    uint32_t width = (uint32_t)(q_max - q_min) + 1u;
    uint32_t height = (uint32_t)(r_max - r_min) + 1u;
    if (out_width) {
        *out_width = (uint16_t)width;
    }
    if (out_height) {
        *out_height = (uint16_t)height;
    }
    return (size_t)width * (size_t)height;
}

static void assign_default_tile(HexTile *tile) {
    tile->terrain = HEX_TERRAIN_OPEN;
    tile->nectar_capacity = 0.0f;
    tile->nectar_stock = 0.0f;
    tile->nectar_recharge_rate = 0.0f;
    tile->flow_capacity = 10.0f;
    tile->flags = HEX_TILE_FLAG_VISIBLE;
}

bool hex_world_init(HexWorld *world, const Params *params) {
    if (!world || !params) {
        LOG_ERROR("hex_world_init received null argument");
        return false;
    }

    memset(world, 0, sizeof(*world));

    const float cell_size = params->hex.cell_size > 0.0f ? params->hex.cell_size : 1.0f;
    world->cell_size = cell_size;
    world->q_min = (int16_t)params->hex.q_min;
    world->q_max = (int16_t)params->hex.q_max;
    world->r_min = (int16_t)params->hex.r_min;
    world->r_max = (int16_t)params->hex.r_max;

    size_t tile_count = hex_tile_count_from_bounds(world->q_min, world->q_max, world->r_min, world->r_max, &world->width, &world->height);
    if (tile_count == 0) {
        LOG_WARN("hex_world_init: empty bounds, skipping allocation");
        return true;
    }

    world->tiles = (HexTile *)calloc(tile_count, sizeof(HexTile));
    if (!world->tiles) {
        LOG_ERROR("hex_world_init: failed to allocate %zu tiles", tile_count);
        memset(world, 0, sizeof(*world));
        return false;
    }

    const float sqrt3 = sqrtf(3.0f);
    const float hive_x = params->hive.rect_x;
    const float hive_y = params->hive.rect_y;
    const float hive_w = params->hive.rect_w;
    const float hive_h = params->hive.rect_h;
    const float hive_center_x = hive_x + hive_w * 0.5f;
    const float hive_center_y = hive_y + hive_h * 0.5f;
    const int entrance_side = params->hive.entrance_side;
    const float entrance_t = params->hive.entrance_t;
    const float entrance_width = params->hive.entrance_width;

    float entrance_center_x = hive_center_x;
    float entrance_center_y = hive_center_y;
    switch (entrance_side) {
        case 0:  // top
            entrance_center_x = hive_x + entrance_t * hive_w;
            entrance_center_y = hive_y;
            break;
        case 1:  // bottom
            entrance_center_x = hive_x + entrance_t * hive_w;
            entrance_center_y = hive_y + hive_h;
            break;
        case 2:  // left
            entrance_center_x = hive_x;
            entrance_center_y = hive_y + entrance_t * hive_h;
            break;
        case 3:  // right
            entrance_center_x = hive_x + hive_w;
            entrance_center_y = hive_y + entrance_t * hive_h;
            break;
        default:
            break;
    }

    const float entrance_half_w = entrance_width * 0.5f;
    const float entrance_padding = cell_size * 0.6f;
    const float hive_padding = cell_size * 0.3f;

    size_t tile_index = 0;
    for (int16_t r = world->r_min; r <= world->r_max; ++r) {
        for (int16_t q = world->q_min; q <= world->q_max; ++q) {
            HexTile *tile = &world->tiles[tile_index++];
            tile->q = q;
            tile->r = r;
            assign_default_tile(tile);

            float cx = cell_size * sqrt3 * ((float)q + (float)r * 0.5f);
            float cy = cell_size * 1.5f * (float)r;
            tile->center_x = cx;
            tile->center_y = cy;

            bool inside_hive = (cx >= hive_x - hive_padding) && (cx <= hive_x + hive_w + hive_padding) &&
                               (cy >= hive_y - hive_padding) && (cy <= hive_y + hive_h + hive_padding) &&
                               (hive_w > 0.0f) && (hive_h > 0.0f);
            if (inside_hive) {
                tile->terrain = HEX_TERRAIN_HIVE;
                tile->flow_capacity = 50.0f;
                tile->nectar_capacity = 0.0f;
                tile->nectar_stock = 0.0f;
                tile->nectar_recharge_rate = 0.0f;
            }

            bool near_entrance = false;
            if (entrance_width > 0.0f) {
                float dx = fabsf(cx - entrance_center_x);
                float dy = fabsf(cy - entrance_center_y);
                switch (entrance_side) {
                    case 0:
                        near_entrance = (dy <= entrance_padding) && (dx <= entrance_half_w + entrance_padding);
                        break;
                    case 1:
                        near_entrance = (dy <= entrance_padding) && (dx <= entrance_half_w + entrance_padding);
                        break;
                    case 2:
                        near_entrance = (dx <= entrance_padding) && (dy <= entrance_half_w + entrance_padding);
                        break;
                    case 3:
                        near_entrance = (dx <= entrance_padding) && (dy <= entrance_half_w + entrance_padding);
                        break;
                    default:
                        break;
                }
            }
            if (near_entrance) {
                tile->terrain = HEX_TERRAIN_ENTRANCE;
                tile->flow_capacity = 75.0f;
                tile->nectar_capacity = 0.0f;
                tile->nectar_stock = 0.0f;
                tile->nectar_recharge_rate = 0.0f;
            }

            if (tile->terrain == HEX_TERRAIN_OPEN) {
                float dx = cx - hive_center_x;
                float dy = cy - hive_center_y;
                float dist = sqrtf(dx * dx + dy * dy);
                if (dist > cell_size * 4.0f) {
                    float ridge = sinf((float)q * 0.7f) * cosf((float)r * 0.7f);
                    if (ridge > 0.45f) {
                        tile->terrain = HEX_TERRAIN_FOREST;
                        tile->nectar_capacity = 4.0f;
                        tile->nectar_stock = tile->nectar_capacity * 0.25f;
                        tile->nectar_recharge_rate = 0.05f;
                    } else if (ridge < -0.55f) {
                        tile->terrain = HEX_TERRAIN_WATER;
                        tile->flow_capacity = 5.0f;
                        tile->nectar_capacity = 0.0f;
                        tile->nectar_stock = 0.0f;
                        tile->nectar_recharge_rate = 0.0f;
                    } else {
                        float bloom = sinf((float)q * 1.1f) + cosf((float)r * 0.9f);
                        if (bloom > 1.2f) {
                            tile->terrain = HEX_TERRAIN_FLOWERS;
                            tile->nectar_capacity = 8.0f;
                            tile->nectar_stock = tile->nectar_capacity;
                            tile->nectar_recharge_rate = 0.15f;
                        } else if (bloom < -1.4f) {
                            tile->terrain = HEX_TERRAIN_MOUNTAIN;
                            tile->flow_capacity = 2.0f;
                            tile->nectar_capacity = 0.0f;
                            tile->nectar_stock = 0.0f;
                            tile->nectar_recharge_rate = 0.0f;
                        }
                    }
                } else {
                    tile->nectar_capacity = 1.0f;
                    tile->nectar_stock = tile->nectar_capacity * 0.1f;
                    tile->nectar_recharge_rate = 0.02f;
                }
            }
        }
    }

    return true;
}

void hex_world_shutdown(HexWorld *world) {
    if (!world) {
        return;
    }
    free(world->tiles);
    memset(world, 0, sizeof(*world));
}

size_t hex_world_tile_count(const HexWorld *world) {
    if (!world || !world->tiles) {
        return 0;
    }
    return (size_t)world->width * (size_t)world->height;
}

bool hex_world_in_bounds(const HexWorld *world, int q, int r) {
    if (!world || !world->tiles) {
        return false;
    }
    if (q < world->q_min || q > world->q_max) {
        return false;
    }
    if (r < world->r_min || r > world->r_max) {
        return false;
    }
    return true;
}

size_t hex_world_index(const HexWorld *world, int q, int r) {
    if (!hex_world_in_bounds(world, q, r)) {
        return (size_t)-1;
    }
    size_t col = (size_t)(q - world->q_min);
    size_t row = (size_t)(r - world->r_min);
    return row * (size_t)world->width + col;
}

HexTile *hex_world_tile_at(HexWorld *world, int q, int r) {
    if (!world || !world->tiles) {
        return NULL;
    }
    size_t index = hex_world_index(world, q, r);
    if (index == (size_t)-1) {
        return NULL;
    }
    return &world->tiles[index];
}

const HexTile *hex_world_const_tile_at(const HexWorld *world, int q, int r) {
    if (!world || !world->tiles) {
        return NULL;
    }
    size_t index = hex_world_index(world, q, r);
    if (index == (size_t)-1) {
        return NULL;
    }
    return &world->tiles[index];
}

void hex_world_axial_to_world(const HexWorld *world, int q, int r, float *out_x, float *out_y) {
    if (!world) {
        if (out_x) {
            *out_x = 0.0f;
        }
        if (out_y) {
            *out_y = 0.0f;
        }
        return;
    }
    const float sqrt3 = sqrtf(3.0f);
    float cx = world->cell_size * sqrt3 * ((float)q + (float)r * 0.5f);
    float cy = world->cell_size * 1.5f * (float)r;
    if (out_x) {
        *out_x = cx;
    }
    if (out_y) {
        *out_y = cy;
    }
}

void hex_world_world_to_axial(const HexWorld *world, float x, float y, float *out_qf, float *out_rf) {
    if (!world || world->cell_size <= 0.0f) {
        if (out_qf) {
            *out_qf = 0.0f;
        }
        if (out_rf) {
            *out_rf = 0.0f;
        }
        return;
    }
    const float R = world->cell_size;
    const float qf = ((sqrtf(3.0f) / 3.0f) * x - (1.0f / 3.0f) * y) / R;
    const float rf = ((2.0f / 3.0f) * y) / R;
    if (out_qf) {
        *out_qf = qf;
    }
    if (out_rf) {
        *out_rf = rf;
    }
}

void hex_world_round_axial(float qf, float rf, int *out_q, int *out_r) {
    float xf = qf;
    float zf = -qf - rf;
    float yf = rf;

    int rx = (int)lrintf(xf);
    int ry = (int)lrintf(yf);
    int rz = (int)lrintf(zf);

    float dx = fabsf((float)rx - xf);
    float dy = fabsf((float)ry - yf);
    float dz = fabsf((float)rz - zf);

    if (dx > dy && dx > dz) {
        rx = -ry - rz;
    } else if (dy > dz) {
        ry = -rx - rz;
    }

    if (out_q) {
        *out_q = rx;
    }
    if (out_r) {
        *out_r = ry;
    }
}

bool hex_world_pick(const HexWorld *world, float x, float y, int *out_q, int *out_r, size_t *out_index) {
    if (out_index) {
        *out_index = (size_t)-1;
    }
    if (out_q) {
        *out_q = 0;
    }
    if (out_r) {
        *out_r = 0;
    }
    if (!world || !world->tiles || world->cell_size <= 0.0f) {
        return false;
    }

    float qf = 0.0f;
    float rf = 0.0f;
    hex_world_world_to_axial(world, x, y, &qf, &rf);

    int q = 0;
    int r = 0;
    hex_world_round_axial(qf, rf, &q, &r);
    if (!hex_world_in_bounds(world, q, r)) {
        return false;
    }

    size_t index = hex_world_index(world, q, r);
    if (index == (size_t)-1) {
        return false;
    }

    const HexTile *tile = &world->tiles[index];
    if (!(tile->flags & HEX_TILE_FLAG_VISIBLE)) {
        return false;
    }

    if (out_q) {
        *out_q = q;
    }
    if (out_r) {
        *out_r = r;
    }
    if (out_index) {
        *out_index = index;
    }
    return true;
}
