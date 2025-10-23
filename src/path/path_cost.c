#include "path/path_cost.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hex.h"
#include "path/path_internal.h"
#include "util/log.h"

static const float kMinEffCost = 1e-3f;
static const float kMaxEffCost = 1e6f;

static const HexWorld *g_cost_world = NULL;
static size_t g_tile_count = 0u;
static float *g_base_cost = NULL;
static float *g_flow_capacity = NULL;
static float *g_crowd_density = NULL;
static float *g_hazard_penalty = NULL;
static float *g_eff_cost = NULL;
static uint8_t *g_dirty_flags = NULL;
static TileId *g_dirty_list = NULL;
static size_t g_dirty_count = 0u;
static size_t g_dirty_capacity = 0u;
static bool g_initialized = false;

static float g_alpha_congestion = 1.0f;
static float g_gamma_hazard = 2.0f;
static float g_ema_lambda = 0.2f;
static float g_dirty_epsilon = 0.1f;

static inline float clamp_cost(float value) {
    if (value < kMinEffCost) {
        return kMinEffCost;
    }
    if (value > kMaxEffCost) {
        return kMaxEffCost;
    }
    return value;
}

static bool ensure_dirty_capacity(size_t needed) {
    if (g_dirty_capacity >= needed) {
        return true;
    }
    size_t new_capacity = g_dirty_capacity ? g_dirty_capacity : 256u;
    while (new_capacity < needed) {
        new_capacity *= 2u;
    }
    TileId *new_list = (TileId *)realloc(g_dirty_list, new_capacity * sizeof(TileId));
    if (!new_list) {
        LOG_ERROR("path_cost: failed to grow dirty queue (capacity=%zu)", new_capacity);
        return false;
    }
    g_dirty_list = new_list;
    g_dirty_capacity = new_capacity;
    return true;
}

static void clear_dirty_queue(void) {
    g_dirty_count = 0u;
}

static void enqueue_dirty(TileId nid) {
    if (!g_initialized || nid >= (TileId)g_tile_count) {
        return;
    }
    if (g_dirty_flags[nid]) {
        return;
    }
    if (!ensure_dirty_capacity(g_dirty_count + 1u)) {
        return;
    }
    g_dirty_list[g_dirty_count++] = nid;
    g_dirty_flags[nid] = 1u;
}

static float compute_congestion_penalty(size_t index) {
    float capacity = g_flow_capacity ? g_flow_capacity[index] : 1.0f;
    if (capacity <= 1e-4f) {
        capacity = 1.0f;
    }
    float density = g_crowd_density ? g_crowd_density[index] : 0.0f;
    float rho = density / capacity;
    if (rho <= 1.0f) {
        return 0.0f;
    }
    float diff = rho - 1.0f;
    return diff * diff;
}

static float compute_eff_cost(size_t index) {
    float base = g_base_cost ? g_base_cost[index] : 1.0f;
    if (!g_cost_world || index >= g_tile_count) {
        return clamp_cost(base);
    }
    const HexTile *tile = &g_cost_world->tiles[index];
    if (!tile->passable) {
        return kMaxEffCost;
    }
    float hazard = g_hazard_penalty ? g_hazard_penalty[index] : 0.0f;
    float congestion = compute_congestion_penalty(index);
    float eff = base + g_alpha_congestion * congestion + g_gamma_hazard * hazard;
    if (!isfinite(eff)) {
        eff = kMaxEffCost;
    }
    return clamp_cost(eff);
}

static void update_eff_cost(size_t index, bool force_dirty) {
    if (!g_eff_cost || index >= g_tile_count) {
        return;
    }
    float old_cost = g_eff_cost[index];
    float new_cost = compute_eff_cost(index);
    g_eff_cost[index] = new_cost;
    if (!force_dirty) {
        float delta = fabsf(new_cost - old_cost);
        float ref = fabsf(old_cost);
        if (ref < 1e-4f) {
            ref = 1e-4f;
        }
        if (delta < ref * g_dirty_epsilon) {
            return;
        }
    }
    enqueue_dirty((TileId)index);
}

static void recompute_all_costs(bool force_dirty) {
    if (!g_initialized || !g_eff_cost) {
        return;
    }
    for (size_t i = 0; i < g_tile_count; ++i) {
        update_eff_cost(i, force_dirty);
    }
}

bool path_cost_init(const HexWorld *world) {
    path_cost_shutdown();
    if (!world) {
        return false;
    }
    size_t tile_count = hex_world_tile_count(world);
    if (tile_count == 0) {
        g_cost_world = world;
        g_initialized = true;
        return true;
    }

    g_base_cost = (float *)malloc(tile_count * sizeof(float));
    g_flow_capacity = (float *)malloc(tile_count * sizeof(float));
    g_crowd_density = (float *)calloc(tile_count, sizeof(float));
    g_hazard_penalty = (float *)calloc(tile_count, sizeof(float));
    g_eff_cost = (float *)malloc(tile_count * sizeof(float));
    g_dirty_flags = (uint8_t *)calloc(tile_count, sizeof(uint8_t));

    if (!g_base_cost || !g_flow_capacity || !g_crowd_density || !g_hazard_penalty || !g_eff_cost ||
        !g_dirty_flags) {
        LOG_ERROR("path_cost: failed to allocate cost buffers (%zu tiles)", tile_count);
        path_cost_shutdown();
        return false;
    }

    for (size_t i = 0; i < tile_count; ++i) {
        const HexTile *tile = &world->tiles[i];
        float base = tile->base_cost;
        if (!tile->passable) {
            base = kMaxEffCost;
        }
        if (!(base > 0.0f)) {
            base = 1.0f;
        }
        g_base_cost[i] = clamp_cost(base);
        float capacity = tile->flow_capacity;
        if (!(capacity > 0.0f)) {
            capacity = 1.0f;
        }
        g_flow_capacity[i] = capacity;
        g_eff_cost[i] = compute_eff_cost(i);
    }

    g_cost_world = world;
    g_tile_count = tile_count;
    g_initialized = true;
    clear_dirty_queue();
    return true;
}

void path_cost_shutdown(void) {
    free(g_base_cost);
    free(g_flow_capacity);
    free(g_crowd_density);
    free(g_hazard_penalty);
    free(g_eff_cost);
    free(g_dirty_flags);
    free(g_dirty_list);

    g_base_cost = NULL;
    g_flow_capacity = NULL;
    g_crowd_density = NULL;
    g_hazard_penalty = NULL;
    g_eff_cost = NULL;
    g_dirty_flags = NULL;
    g_dirty_list = NULL;
    g_dirty_capacity = 0u;
    g_dirty_count = 0u;
    g_cost_world = NULL;
    g_tile_count = 0u;
    g_initialized = false;
}

const float *path_cost_eff_costs(void) {
    return g_eff_cost;
}

size_t path_cost_tile_count(void) {
    return g_tile_count;
}

size_t path_cost_dirty_count(void) {
    return g_dirty_count;
}

size_t path_cost_consume_dirty(TileId *out, size_t max_tiles) {
    if (!g_initialized || g_dirty_count == 0u || !out || max_tiles == 0u) {
        return 0u;
    }
    size_t count = g_dirty_count;
    if (count > max_tiles) {
        count = max_tiles;
    }
    memcpy(out, g_dirty_list, count * sizeof(TileId));
    for (size_t i = 0; i < count; ++i) {
        TileId nid = out[i];
        if ((size_t)nid < g_tile_count) {
            g_dirty_flags[nid] = 0u;
        }
    }
    if (count < g_dirty_count) {
        size_t remain = g_dirty_count - count;
        memmove(g_dirty_list, g_dirty_list + count, remain * sizeof(TileId));
        g_dirty_count = remain;
    } else {
        g_dirty_count = 0u;
    }
    return count;
}

void path_cost_requeue_tiles(const TileId *tiles, size_t count) {
    if (!tiles) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        path_cost_mark_dirty(tiles[i]);
    }
}

void path_cost_set_coeffs(float alpha_congestion, float gamma_hazard) {
    if (alpha_congestion < 0.0f) {
        alpha_congestion = 0.0f;
    }
    if (gamma_hazard < 0.0f) {
        gamma_hazard = 0.0f;
    }
    if (fabsf(g_alpha_congestion - alpha_congestion) < 1e-6f &&
        fabsf(g_gamma_hazard - gamma_hazard) < 1e-6f) {
        return;
    }
    g_alpha_congestion = alpha_congestion;
    g_gamma_hazard = gamma_hazard;
    recompute_all_costs(false);
}

void path_cost_set_ema_lambda(float lambda) {
    if (lambda < 0.0f) {
        lambda = 0.0f;
    }
    if (lambda > 1.0f) {
        lambda = 1.0f;
    }
    g_ema_lambda = lambda;
}

void path_cost_set_dirty_threshold(float relative_eps) {
    if (relative_eps < 0.0f) {
        relative_eps = 0.0f;
    }
    g_dirty_epsilon = relative_eps;
}

void path_cost_set_hazard(TileId nid, float penalty) {
    if (!g_initialized || !g_hazard_penalty || nid < 0 || (size_t)nid >= g_tile_count) {
        return;
    }
    if (penalty < 0.0f) {
        penalty = 0.0f;
    }
    float old_penalty = g_hazard_penalty[nid];
    if (fabsf(old_penalty - penalty) < 1e-6f) {
        return;
    }
    g_hazard_penalty[nid] = penalty;
    update_eff_cost((size_t)nid, false);
}

void path_cost_add_crowd_samples(const TileId *tiles, const float *bees_per_sec, int count) {
    if (!g_initialized || !g_crowd_density || !tiles || !bees_per_sec || count <= 0) {
        return;
    }
    float lambda = g_ema_lambda;
    for (int i = 0; i < count; ++i) {
        TileId nid = tiles[i];
        if (nid < 0 || (size_t)nid >= g_tile_count) {
            continue;
        }
        float sample = bees_per_sec[i];
        if (sample < 0.0f) {
            sample = 0.0f;
        }
        float prev = g_crowd_density[nid];
        float updated = sample;
        if (lambda <= 0.0f) {
            updated = prev;
        } else if (lambda >= 1.0f) {
            updated = sample;
        } else {
            updated = prev + lambda * (sample - prev);
        }
        if (fabsf(updated - prev) < 1e-6f) {
            continue;
        }
        g_crowd_density[nid] = updated;
        update_eff_cost((size_t)nid, false);
    }
}

void path_cost_mark_dirty(TileId nid) {
    if (!g_initialized || nid < 0 || (size_t)nid >= g_tile_count) {
        return;
    }
    enqueue_dirty(nid);
}

void path_cost_mark_many_dirty(const TileId *tiles, int count) {
    if (!tiles || count <= 0) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        path_cost_mark_dirty(tiles[i]);
    }
}

