#include "path/path.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "path/path_internal.h"
#include "path/path_scheduler.h"
#include "util/log.h"

static const int kAxialDirs[6][2] = {
    {1, 0},
    {1, -1},
    {0, -1},
    {-1, 0},
    {-1, 1},
    {0, 1},
};

static const HexWorld *g_world = NULL;
static size_t g_tile_count = 0;
static int32_t *g_neighbors = NULL;
static TileId *g_goal_entrance = NULL;
static size_t g_goal_entrance_count = 0;
static TileId *g_goal_unload = NULL;
static size_t g_goal_unload_count = 0;
static TileId *g_goal_flowers = NULL;
static float *g_goal_flowers_seed = NULL;
static float *g_goal_flowers_seed_lut = NULL;
static uint8_t *g_goal_flowers_membership = NULL;
static size_t g_goal_flowers_count = 0u;
static float g_flowers_refresh_accum = 0.0f;
static float g_dir_world[6][2] = {{0}};
static bool g_path_initialized = false;

static const float kFlowersRefreshIntervalSec = 0.35f;
static const float kFlowersThetaOn = 0.05f;
static const float kFlowersThetaOff = 0.02f;
static const float kFlowersSeedBias = 1.0f;
static const float kFlowersWeightStock = 0.7f;
static const float kFlowersWeightQuality = 0.3f;

static void clear_core_storage(void) {
    free(g_neighbors);
    g_neighbors = NULL;
    free(g_goal_entrance);
    g_goal_entrance = NULL;
    free(g_goal_unload);
    g_goal_unload = NULL;
    free(g_goal_flowers);
    free(g_goal_flowers_seed);
    free(g_goal_flowers_seed_lut);
    free(g_goal_flowers_membership);
    g_goal_flowers = NULL;
    g_goal_flowers_seed = NULL;
    g_goal_flowers_seed_lut = NULL;
    g_goal_flowers_membership = NULL;
    g_tile_count = 0;
    g_goal_entrance_count = 0;
    g_goal_unload_count = 0;
    g_goal_flowers_count = 0u;
    g_world = NULL;
    memset(g_dir_world, 0, sizeof(g_dir_world));
    g_flowers_refresh_accum = 0.0f;
}

static bool compute_direction_table(const HexWorld *world) {
    if (!world) {
        return false;
    }
    float base_x = 0.0f;
    float base_y = 0.0f;
    hex_world_axial_to_world(world, 0, 0, &base_x, &base_y);
    for (int dir = 0; dir < 6; ++dir) {
        int dq = kAxialDirs[dir][0];
        int dr = kAxialDirs[dir][1];
        float neighbor_x = 0.0f;
        float neighbor_y = 0.0f;
        hex_world_axial_to_world(world, dq, dr, &neighbor_x, &neighbor_y);
        float dx = neighbor_x - base_x;
        float dy = neighbor_y - base_y;
        float len = sqrtf(dx * dx + dy * dy);
        if (len > 0.0f) {
            dx /= len;
            dy /= len;
        } else {
            dx = 0.0f;
            dy = 0.0f;
        }
        g_dir_world[dir][0] = dx;
        g_dir_world[dir][1] = dy;
    }
    return true;
}

static bool build_neighbors(const HexWorld *world) {
    size_t tile_count = hex_world_tile_count(world);
    if (tile_count == 0) {
        return true;
    }
    int32_t *neighbors = (int32_t *)malloc(tile_count * 6u * sizeof(int32_t));
    if (!neighbors) {
        LOG_ERROR("path: failed to allocate neighbor table (%zu tiles)", tile_count);
        return false;
    }
    for (size_t i = 0; i < tile_count * 6u; ++i) {
        neighbors[i] = -1;
    }

    for (size_t index = 0; index < tile_count; ++index) {
        if (!hex_world_tile_passable(world, index)) {
            continue;
        }
        int q = 0;
        int r = 0;
        if (!hex_world_index_to_axial(world, index, &q, &r)) {
            continue;
        }
        for (int dir = 0; dir < 6; ++dir) {
            int nq = q + kAxialDirs[dir][0];
            int nr = r + kAxialDirs[dir][1];
            if (!hex_world_in_bounds(world, nq, nr)) {
                continue;
            }
            size_t neighbor_index = hex_world_index(world, nq, nr);
            if (!hex_world_tile_passable(world, neighbor_index)) {
                continue;
            }
            neighbors[index * 6u + (size_t)dir] = (int32_t)neighbor_index;
        }
    }

    g_neighbors = neighbors;
    return true;
}

static bool build_entrance_goals(const HexWorld *world) {
    size_t tile_count = hex_world_tile_count(world);
    if (tile_count == 0) {
        g_goal_entrance_count = 0;
        return false;
    }
    TileId *goals = (TileId *)malloc(tile_count * sizeof(TileId));
    if (!goals) {
        LOG_ERROR("path: failed to allocate goal buffer (%zu tiles)", tile_count);
        return false;
    }
    size_t count = 0;
    for (size_t index = 0; index < tile_count; ++index) {
        const HexTile *tile = &world->tiles[index];
        if (tile->terrain == HEX_TERRAIN_HIVE_ENTRANCE) {
            goals[count++] = index;
        }
    }
    if (count == 0) {
        LOG_ERROR("path: no entrance tiles found; path field unavailable");
        free(goals);
        return false;
    }
    g_goal_entrance = goals;
    g_goal_entrance_count = count;
    return true;
}

static bool build_unload_goals(const HexWorld *world) {
    size_t tile_count = hex_world_tile_count(world);
    if (tile_count == 0) {
        g_goal_unload = NULL;
        g_goal_unload_count = 0;
        return false;
    }
    TileId *goals = (TileId *)malloc(tile_count * sizeof(TileId));
    if (!goals) {
        LOG_ERROR("path: failed to allocate unload goal buffer (%zu tiles)", tile_count);
        return false;
    }
    size_t count = 0;
    for (size_t index = 0; index < tile_count; ++index) {
        const HexTile *tile = &world->tiles[index];
        if (tile->terrain == HEX_TERRAIN_HIVE_STORAGE) {
            goals[count++] = index;
        }
    }
    if (count == 0) {
        free(goals);
        g_goal_unload = NULL;
        g_goal_unload_count = 0;
        LOG_WARN("path: no unload/storage tiles found; unload field disabled");
        return true;
    }
    TileId *shrink = (TileId *)realloc(goals, count * sizeof(TileId));
    g_goal_unload = shrink ? shrink : goals;
    g_goal_unload_count = count;
    return true;
}

static bool allocate_flowers_storage(size_t tile_count) {
    free(g_goal_flowers);
    free(g_goal_flowers_seed);
    free(g_goal_flowers_seed_lut);
    free(g_goal_flowers_membership);
    g_goal_flowers = NULL;
    g_goal_flowers_seed = NULL;
    g_goal_flowers_seed_lut = NULL;
    g_goal_flowers_membership = NULL;
    g_goal_flowers_count = 0u;
    if (tile_count == 0u) {
        return true;
    }
    g_goal_flowers = (TileId *)malloc(tile_count * sizeof(TileId));
    g_goal_flowers_seed = (float *)malloc(tile_count * sizeof(float));
    g_goal_flowers_seed_lut = (float *)calloc(tile_count, sizeof(float));
    g_goal_flowers_membership = (uint8_t *)calloc(tile_count, sizeof(uint8_t));
    if (!g_goal_flowers || !g_goal_flowers_seed || !g_goal_flowers_seed_lut || !g_goal_flowers_membership) {
        LOG_ERROR("path: failed to allocate flowers goal buffers (%zu tiles)", tile_count);
        free(g_goal_flowers);
        free(g_goal_flowers_seed);
        free(g_goal_flowers_seed_lut);
        free(g_goal_flowers_membership);
        g_goal_flowers = NULL;
        g_goal_flowers_seed = NULL;
        g_goal_flowers_seed_lut = NULL;
        g_goal_flowers_membership = NULL;
        g_goal_flowers_count = 0u;
        return false;
    }
    return true;
}

static bool refresh_flowers_goals(const HexWorld *world, bool *out_changed) {
    if (out_changed) {
        *out_changed = false;
    }
    if (!world || !g_goal_flowers || !g_goal_flowers_seed || !g_goal_flowers_membership) {
        g_goal_flowers_count = 0u;
        return false;
    }
    if (g_tile_count == 0u) {
        g_goal_flowers_count = 0u;
        return true;
    }
    bool membership_changed = false;
    bool seed_changed = false;
    size_t write_index = 0u;
    for (size_t index = 0; index < g_tile_count; ++index) {
        const HexTile *tile = &world->tiles[index];
        bool was_goal = g_goal_flowers_membership[index] != 0u;
        bool now_goal = false;
        float seed_value = 0.0f;
        bool candidate = tile->terrain == HEX_TERRAIN_FLOWERS && tile->passable;
        float stock_ratio = 0.0f;
        if (candidate) {
            float capacity = tile->nectar_capacity;
            if (capacity > 1e-3f) {
                stock_ratio = tile->nectar_stock / capacity;
            } else if (tile->nectar_stock > 0.0f) {
                stock_ratio = 1.0f;
            }
            if (stock_ratio < 0.0f) {
                stock_ratio = 0.0f;
            }
            if (stock_ratio > 1.0f) {
                stock_ratio = 1.0f;
            }
            if (was_goal) {
                if (stock_ratio > kFlowersThetaOff) {
                    now_goal = true;
                }
            } else if (stock_ratio >= kFlowersThetaOn) {
                now_goal = true;
            }
            if (now_goal) {
                float quality = tile->flower_quality;
                if (quality < 0.0f) {
                    quality = 0.0f;
                }
                if (quality > 1.0f) {
                    quality = 1.0f;
                }
                float desirability = kFlowersWeightStock * stock_ratio +
                                     kFlowersWeightQuality * quality;
                if (desirability < 0.0f) {
                    desirability = 0.0f;
                }
                if (desirability > 1.0f) {
                    desirability = 1.0f;
                }
                seed_value = kFlowersSeedBias * (1.0f - desirability);
                if (!(seed_value >= 0.0f) || !isfinite(seed_value)) {
                    seed_value = 0.0f;
                }
            }
        }
        if (now_goal != was_goal) {
            membership_changed = true;
        }
        if (now_goal) {
            if (g_goal_flowers_seed_lut) {
                float prev_seed = g_goal_flowers_seed_lut[index];
                if (!seed_changed && fabsf(prev_seed - seed_value) > 1e-4f) {
                    seed_changed = true;
                }
                g_goal_flowers_seed_lut[index] = seed_value;
            }
            g_goal_flowers_membership[index] = 1u;
            g_goal_flowers[write_index] = (TileId)index;
            g_goal_flowers_seed[write_index] = seed_value;
            ++write_index;
        } else {
            if (g_goal_flowers_seed_lut) {
                g_goal_flowers_seed_lut[index] = 0.0f;
            }
            g_goal_flowers_membership[index] = 0u;
        }
    }
    if (write_index != g_goal_flowers_count) {
        membership_changed = true;
    }
    g_goal_flowers_count = write_index;
    if (out_changed) {
        *out_changed = membership_changed || seed_changed;
    }
    return true;
}

const float *path_core_direction_world(uint8_t dir_index) {
    if (dir_index >= 6u) {
        return NULL;
    }
    return g_dir_world[dir_index];
}

bool path_init(const HexWorld *world, const Params *params) {
    (void)params;
    if (!world) {
        LOG_ERROR("path: init requires valid world");
        return false;
    }

    path_shutdown();

    g_world = world;
    g_tile_count = hex_world_tile_count(world);

    path_sched_reset_state();

    if (!compute_direction_table(world)) {
        LOG_ERROR("path: failed to compute direction vectors");
        path_shutdown();
        return false;
    }

    if (!build_neighbors(world)) {
        path_shutdown();
        return false;
    }

    if (!build_entrance_goals(world)) {
        path_shutdown();
        return false;
    }

    if (!build_unload_goals(world)) {
        LOG_WARN("path: unload goal build failed; unload field disabled");
        g_goal_unload_count = 0;
    }

    if (!path_cost_init(world)) {
        LOG_ERROR("path: failed to initialize cost buffers");
        path_shutdown();
        return false;
    }

    if (!path_fields_init_storage(g_tile_count)) {
        path_shutdown();
        return false;
    }

    if (!allocate_flowers_storage(g_tile_count)) {
        path_shutdown();
        return false;
    }
    g_flowers_refresh_accum = 0.0f;
    if (!refresh_flowers_goals(world, NULL)) {
        LOG_WARN("path: failed to prepare flowers goal set; flowers field disabled");
        g_goal_flowers_count = 0u;
    }

    if (!path_fields_start_build(PATH_GOAL_ENTRANCE,
                                 world,
                                 g_neighbors,
                                 g_goal_entrance,
                                 g_goal_entrance_count,
                                 NULL,
                                 path_cost_eff_costs(),
                                 NULL,
                                 0u)) {
        LOG_ERROR("path: failed to start entrance field build");
        path_shutdown();
        return false;
    }

    bool finished = false;
    while (!finished) {
        if (!path_fields_step(PATH_GOAL_ENTRANCE, 1000000.0, NULL, NULL, &finished)) {
            LOG_ERROR("path: failed while building entrance field");
            path_shutdown();
            return false;
        }
    }

    if (g_goal_unload_count > 0u) {
        bool started_unload = path_fields_start_build(PATH_GOAL_UNLOAD,
                                                      world,
                                                      g_neighbors,
                                                      g_goal_unload,
                                                      g_goal_unload_count,
                                                      NULL,
                                                      path_cost_eff_costs(),
                                                      NULL,
                                                      0u);
        if (started_unload) {
            bool unload_finished = false;
            while (!unload_finished) {
                if (!path_fields_step(PATH_GOAL_UNLOAD, 1000000.0, NULL, NULL, &unload_finished)) {
                    LOG_ERROR("path: failed while building unload field");
                    g_goal_unload_count = 0u;
                    break;
                }
            }
        } else {
            LOG_WARN("path: unable to build unload field; using entrance field only");
            g_goal_unload_count = 0u;
        }
    }

    if (g_goal_flowers_count > 0u) {
        bool started_flowers = path_fields_start_build(PATH_GOAL_FLOWERS_NEAR,
                                                       world,
                                                       g_neighbors,
                                                       g_goal_flowers,
                                                       g_goal_flowers_count,
                                                       g_goal_flowers_seed,
                                                       path_cost_eff_costs(),
                                                       NULL,
                                                       0u);
        if (started_flowers) {
            bool flowers_finished = false;
            while (!flowers_finished) {
                if (!path_fields_step(PATH_GOAL_FLOWERS_NEAR, 1000000.0, NULL, NULL, &flowers_finished)) {
                    LOG_WARN("path: failed while building flowers field; disabling field");
                    g_goal_flowers_count = 0u;
                    break;
                }
            }
        } else {
            LOG_WARN("path: unable to build flowers field; field disabled");
            g_goal_flowers_count = 0u;
        }
    }

    path_sched_set_goal_data(PATH_GOAL_ENTRANCE,
                             world,
                             g_neighbors,
                             g_goal_entrance,
                             NULL,
                             g_goal_entrance_count,
                             g_tile_count);
    path_sched_set_goal_data(PATH_GOAL_UNLOAD,
                             world,
                             g_neighbors,
                             (g_goal_unload_count > 0u) ? g_goal_unload : NULL,
                             NULL,
                             g_goal_unload_count,
                             g_tile_count);
    path_sched_set_goal_data(PATH_GOAL_FLOWERS_NEAR,
                             world,
                             g_neighbors,
                             (g_goal_flowers_count > 0u) ? g_goal_flowers : NULL,
                             (g_goal_flowers_count > 0u) ? g_goal_flowers_seed : NULL,
                             g_goal_flowers_count,
                             g_tile_count);

    if (!path_debug_init()) {
        LOG_WARN("path: debug overlay initialization failed");
    }
    path_debug_reset_overlay();
    size_t tile_count = path_field_tile_count();
    if (tile_count == g_tile_count && tile_count > 0u) {
        const uint8_t *next = path_field_next(PATH_GOAL_ENTRANCE);
        if (next) {
            path_debug_build_overlay(world, PATH_GOAL_ENTRANCE, next, tile_count, 0x33FF66FFu);
        }
        if (g_goal_unload_count > 0u) {
            const uint8_t *next_unload = path_field_next(PATH_GOAL_UNLOAD);
            if (next_unload) {
                path_debug_build_overlay(world, PATH_GOAL_UNLOAD, next_unload, tile_count, 0xFFAA33FFu);
            }
        }
        if (g_goal_flowers_count > 0u) {
            const uint8_t *next_flowers = path_field_next(PATH_GOAL_FLOWERS_NEAR);
            if (next_flowers) {
                path_debug_build_overlay(world,
                                         PATH_GOAL_FLOWERS_NEAR,
                                         next_flowers,
                                         tile_count,
                                         0xAA66FFFFu);
            }
        }
    }

    g_path_initialized = true;
    LOG_INFO("path: fields built (tiles=%zu entrance_goals=%zu unload_goals=%zu flowers_goals=%zu)",
             g_tile_count,
             g_goal_entrance_count,
             g_goal_unload_count,
             g_goal_flowers_count);
    return true;
}

void path_shutdown(void) {
    if (!g_path_initialized) {
        path_debug_reset_overlay();
    }
    path_debug_shutdown();
    path_sched_shutdown_state();
    path_fields_shutdown_storage();
    path_cost_shutdown();
    clear_core_storage();
    g_path_initialized = false;
}

void path_update(const HexWorld *world, const Params *params, float dt_sec) {
    (void)world;
    (void)params;
    if (!g_path_initialized || !g_world) {
        return;
    }

    if (dt_sec > 0.0f) {
        g_flowers_refresh_accum += dt_sec;
        if (g_flowers_refresh_accum >= kFlowersRefreshIntervalSec) {
            g_flowers_refresh_accum = fmodf(g_flowers_refresh_accum, kFlowersRefreshIntervalSec);
            bool changed = false;
            if (refresh_flowers_goals(g_world, &changed)) {
                if (changed) {
                    const TileId *goals_ptr = (g_goal_flowers_count > 0u) ? g_goal_flowers : NULL;
                    const float *seed_ptr = (g_goal_flowers_count > 0u) ? g_goal_flowers_seed : NULL;
                    path_sched_set_goal_data(PATH_GOAL_FLOWERS_NEAR,
                                             g_world,
                                             g_neighbors,
                                             goals_ptr,
                                             seed_ptr,
                                             g_goal_flowers_count,
                                             g_tile_count);
                    if (g_goal_flowers_count > 0u) {
                        path_sched_force_full_recompute(PATH_GOAL_FLOWERS_NEAR);
                    }
                }
            } else {
                LOG_WARN("path: failed to refresh flowers goal set during update");
            }
        }
    }

    bool swapped[PATH_GOAL_COUNT] = {false};
    if (!path_sched_update(dt_sec, swapped)) {
        return;
    }

    bool rebuild_overlay = false;

    if (swapped[PATH_GOAL_ENTRANCE]) {
        rebuild_overlay = true;
        uint32_t stamp = path_sched_get_stamp(PATH_GOAL_ENTRANCE);
        float build_ms = path_sched_get_last_build_ms(PATH_GOAL_ENTRANCE);
        size_t relaxed = path_sched_get_last_relaxed(PATH_GOAL_ENTRANCE);
        size_t dirty_processed = path_sched_get_dirty_processed_last_build(PATH_GOAL_ENTRANCE);
        if (dirty_processed > 0u || build_ms > 0.0f) {
            LOG_INFO("path: entrance swap stamp=%u build_ms=%.3f relaxed=%zu dirty=%zu",
                     stamp,
                     build_ms,
                     relaxed,
                     dirty_processed);
        }
    }
    if (swapped[PATH_GOAL_UNLOAD]) {
        rebuild_overlay = true;
        uint32_t stamp = path_sched_get_stamp(PATH_GOAL_UNLOAD);
        float build_ms = path_sched_get_last_build_ms(PATH_GOAL_UNLOAD);
        size_t relaxed = path_sched_get_last_relaxed(PATH_GOAL_UNLOAD);
        size_t dirty_processed = path_sched_get_dirty_processed_last_build(PATH_GOAL_UNLOAD);
        if (dirty_processed > 0u || build_ms > 0.0f) {
            LOG_INFO("path: unload swap stamp=%u build_ms=%.3f relaxed=%zu dirty=%zu",
                     stamp,
                     build_ms,
                     relaxed,
                     dirty_processed);
        }
    }
    if (swapped[PATH_GOAL_FLOWERS_NEAR]) {
        rebuild_overlay = true;
        uint32_t stamp = path_sched_get_stamp(PATH_GOAL_FLOWERS_NEAR);
        float build_ms = path_sched_get_last_build_ms(PATH_GOAL_FLOWERS_NEAR);
        size_t relaxed = path_sched_get_last_relaxed(PATH_GOAL_FLOWERS_NEAR);
        size_t dirty_processed = path_sched_get_dirty_processed_last_build(PATH_GOAL_FLOWERS_NEAR);
        if (dirty_processed > 0u || build_ms > 0.0f) {
            LOG_INFO("path: flowers swap stamp=%u build_ms=%.3f relaxed=%zu dirty=%zu",
                     stamp,
                     build_ms,
                     relaxed,
                     dirty_processed);
        }
    }

    if (rebuild_overlay) {
        path_debug_reset_overlay();
        size_t tile_count = path_field_tile_count();
        if (tile_count == g_tile_count && tile_count > 0u) {
            const uint8_t *next_entrance = path_field_next(PATH_GOAL_ENTRANCE);
            if (next_entrance) {
                path_debug_build_overlay(g_world, PATH_GOAL_ENTRANCE, next_entrance, tile_count, 0x33FF66FFu);
            }
            if (g_goal_unload_count > 0u) {
                const uint8_t *next_unload = path_field_next(PATH_GOAL_UNLOAD);
                if (next_unload) {
                    path_debug_build_overlay(g_world, PATH_GOAL_UNLOAD, next_unload, tile_count, 0xFFAA33FFu);
                }
            }
            if (g_goal_flowers_count > 0u) {
                const uint8_t *next_flowers = path_field_next(PATH_GOAL_FLOWERS_NEAR);
                if (next_flowers) {
                    path_debug_build_overlay(g_world,
                                             PATH_GOAL_FLOWERS_NEAR,
                                             next_flowers,
                                             tile_count,
                                             0xAA66FFFFu);
                }
            }
        }
    }
}

void path_force_recompute(PathGoal goal) {
    if (!g_path_initialized || goal < 0 || goal >= PATH_GOAL_COUNT) {
        return;
    }
    path_sched_force_full_recompute(goal);
}

bool path_query_direction(PathGoal goal, TileId nid, PathVec2 *dir_world_out) {
    if (!dir_world_out || !g_world) {
        return false;
    }
    if (goal == PATH_GOAL_UNLOAD && g_goal_unload_count == 0u) {
        goal = PATH_GOAL_ENTRANCE;
    }
    if (goal == PATH_GOAL_FLOWERS_NEAR && g_goal_flowers_count == 0u) {
        return false;
    }
    if (goal < 0 || goal >= PATH_GOAL_COUNT) {
        return false;
    }
    if (nid >= g_tile_count) {
        return false;
    }
    const uint8_t *next = path_field_next(goal);
    if (!next) {
        return false;
    }
    uint8_t dir = next[nid];
    if (dir >= 6u) {
        return false;
    }
    const float *vec = g_dir_world[dir];
    dir_world_out->x = vec[0];
    dir_world_out->y = vec[1];
    return true;
}

