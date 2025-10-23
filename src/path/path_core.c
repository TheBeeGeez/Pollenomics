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
static float g_dir_world[6][2] = {{0}};
static bool g_path_initialized = false;

static void clear_core_storage(void) {
    free(g_neighbors);
    g_neighbors = NULL;
    free(g_goal_entrance);
    g_goal_entrance = NULL;
    free(g_goal_unload);
    g_goal_unload = NULL;
    g_tile_count = 0;
    g_goal_entrance_count = 0;
    g_goal_unload_count = 0;
    g_world = NULL;
    memset(g_dir_world, 0, sizeof(g_dir_world));
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

    if (!path_fields_start_build(PATH_GOAL_ENTRANCE,
                                 world,
                                 g_neighbors,
                                 g_goal_entrance,
                                 g_goal_entrance_count,
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

    path_sched_set_goal_data(PATH_GOAL_ENTRANCE,
                             world,
                             g_neighbors,
                             g_goal_entrance,
                             g_goal_entrance_count,
                             g_tile_count);
    path_sched_set_goal_data(PATH_GOAL_UNLOAD,
                             world,
                             g_neighbors,
                             (g_goal_unload_count > 0u) ? g_goal_unload : NULL,
                             g_goal_unload_count,
                             g_tile_count);

    if (!path_debug_init()) {
        LOG_WARN("path: debug overlay initialization failed");
    }
    path_debug_reset_overlay();
    const uint8_t *next = path_field_next(PATH_GOAL_ENTRANCE);
    size_t tile_count = path_field_tile_count();
    if (next && tile_count == g_tile_count) {
        path_debug_build_overlay(world, PATH_GOAL_ENTRANCE, next, tile_count);
    }

    g_path_initialized = true;
    LOG_INFO("path: fields built (tiles=%zu entrance_goals=%zu unload_goals=%zu)",
             g_tile_count,
             g_goal_entrance_count,
             g_goal_unload_count);
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

    bool swapped[PATH_GOAL_COUNT] = {false};
    if (!path_sched_update(dt_sec, swapped)) {
        return;
    }
    if (swapped[PATH_GOAL_ENTRANCE]) {
        const uint8_t *next = path_field_next(PATH_GOAL_ENTRANCE);
        size_t tile_count = path_field_tile_count();
        if (next && tile_count == g_tile_count) {
            path_debug_reset_overlay();
            if (!path_debug_build_overlay(g_world, PATH_GOAL_ENTRANCE, next, tile_count)) {
                path_debug_reset_overlay();
            }
        }
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

