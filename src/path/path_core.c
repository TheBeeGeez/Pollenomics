#include "path/path.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "path/path_internal.h"
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
static float g_dir_world[6][2] = {{0}};
static bool g_path_initialized = false;

static void clear_core_storage(void) {
    free(g_neighbors);
    g_neighbors = NULL;
    free(g_goal_entrance);
    g_goal_entrance = NULL;
    g_tile_count = 0;
    g_goal_entrance_count = 0;
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

    if (!path_fields_init_storage(g_tile_count)) {
        path_shutdown();
        return false;
    }

    if (!path_fields_build(PATH_GOAL_ENTRANCE, world, g_neighbors, g_goal_entrance, g_goal_entrance_count)) {
        LOG_ERROR("path: failed to build entrance field");
        path_shutdown();
        return false;
    }

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
    LOG_INFO("path: entrance field built (tiles=%zu goals=%zu)", g_tile_count, g_goal_entrance_count);
    return true;
}

void path_shutdown(void) {
    if (!g_path_initialized) {
        path_debug_reset_overlay();
    }
    path_debug_shutdown();
    path_fields_shutdown_storage();
    clear_core_storage();
    g_path_initialized = false;
}

bool path_query_direction(PathGoal goal, TileId nid, PathVec2 *dir_world_out) {
    if (!dir_world_out || goal != PATH_GOAL_ENTRANCE || !g_world) {
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

