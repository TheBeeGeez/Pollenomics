#include "path/path_fields.h"

#include <float.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "path/path_internal.h"
#include "util/log.h"

typedef struct PathNode {
    TileId id;
    float dist;
} PathNode;

typedef struct PathHeap {
    PathNode *data;
    size_t size;
    size_t capacity;
} PathHeap;

typedef struct PathFieldBuildState {
    PathHeap heap;
    float *dist;
    uint8_t *next;
    const int32_t *neighbors;
    const TileId *goals;
    size_t goal_count;
    const float *eff_cost;
    const TileId *dirty_tiles;
    size_t dirty_count;
    bool in_progress;
} PathFieldBuildState;

typedef struct PathFieldGoalState {
    float *dist[2];
    uint8_t *next[2];
    int active_index;
    int build_index;
    uint32_t stamp;
    PathFieldBuildState build;
} PathFieldGoalState;

typedef struct PathFieldState {
    size_t tile_count;
    PathFieldGoalState goals[PATH_GOAL_COUNT];
} PathFieldState;

static PathFieldState g_field_state = {0};

static const float kInf = FLT_MAX / 4.0f;

static double clock_now_ms(void) {
    return (double)clock() * (1000.0 / (double)CLOCKS_PER_SEC);
}

static PathFieldGoalState *field_goal_state(PathGoal goal) {
    if (goal < 0 || goal >= PATH_GOAL_COUNT) {
        return NULL;
    }
    return &g_field_state.goals[goal];
}

static bool heap_reserve(PathHeap *heap, size_t capacity) {
    if (!heap) {
        return false;
    }
    if (capacity <= heap->capacity) {
        return true;
    }
    size_t new_capacity = heap->capacity ? heap->capacity : 64u;
    while (new_capacity < capacity) {
        new_capacity *= 2u;
    }
    PathNode *new_data = (PathNode *)realloc(heap->data, new_capacity * sizeof(PathNode));
    if (!new_data) {
        return false;
    }
    heap->data = new_data;
    heap->capacity = new_capacity;
    return true;
}

static void heap_clear(PathHeap *heap) {
    if (heap) {
        heap->size = 0u;
    }
}

static void heap_swap(PathNode *a, PathNode *b) {
    PathNode tmp = *a;
    *a = *b;
    *b = tmp;
}

static bool heap_push(PathHeap *heap, TileId id, float dist) {
    if (!heap) {
        return false;
    }
    if (!heap_reserve(heap, heap->size + 1u)) {
        return false;
    }
    size_t index = heap->size++;
    heap->data[index].id = id;
    heap->data[index].dist = dist;
    while (index > 0u) {
        size_t parent = (index - 1u) / 2u;
        if (heap->data[parent].dist <= heap->data[index].dist) {
            break;
        }
        heap_swap(&heap->data[parent], &heap->data[index]);
        index = parent;
    }
    return true;
}

static bool heap_pop(PathHeap *heap, PathNode *out_node) {
    if (!heap || heap->size == 0u) {
        return false;
    }
    if (out_node) {
        *out_node = heap->data[0];
    }
    heap->size -= 1u;
    if (heap->size == 0u) {
        return true;
    }
    heap->data[0] = heap->data[heap->size];
    size_t index = 0u;
    while (true) {
        size_t left = index * 2u + 1u;
        size_t right = left + 1u;
        size_t smallest = index;
        if (left < heap->size && heap->data[left].dist < heap->data[smallest].dist) {
            smallest = left;
        }
        if (right < heap->size && heap->data[right].dist < heap->data[smallest].dist) {
            smallest = right;
        }
        if (smallest == index) {
            break;
        }
        heap_swap(&heap->data[index], &heap->data[smallest]);
        index = smallest;
    }
    return true;
}

static uint8_t opposite_direction(uint8_t dir) {
    static const uint8_t kOpposite[6] = {3, 4, 5, 0, 1, 2};
    if (dir < 6u) {
        return kOpposite[dir];
    }
    return 255u;
}

bool path_fields_init_storage(size_t tile_count) {
    if (tile_count == 0u) {
        path_fields_shutdown_storage();
        return true;
    }

    float *new_dist[PATH_GOAL_COUNT][2] = {{0}};
    uint8_t *new_next[PATH_GOAL_COUNT][2] = {{0}};

    for (int goal = 0; goal < PATH_GOAL_COUNT; ++goal) {
        for (int buffer = 0; buffer < 2; ++buffer) {
            new_dist[goal][buffer] = (float *)malloc(tile_count * sizeof(float));
            new_next[goal][buffer] = (uint8_t *)malloc(tile_count * sizeof(uint8_t));
            if (!new_dist[goal][buffer] || !new_next[goal][buffer]) {
                for (int g = 0; g <= goal; ++g) {
                    for (int b = 0; b < 2; ++b) {
                        free(new_dist[g][b]);
                        free(new_next[g][b]);
                    }
                }
                LOG_ERROR("path: failed to allocate field storage (%zu tiles)", tile_count);
                return false;
            }
        }
    }

    for (int goal = 0; goal < PATH_GOAL_COUNT; ++goal) {
        PathFieldGoalState *goal_state = &g_field_state.goals[goal];
        for (int buffer = 0; buffer < 2; ++buffer) {
            free(goal_state->dist[buffer]);
            free(goal_state->next[buffer]);
            goal_state->dist[buffer] = new_dist[goal][buffer];
            goal_state->next[buffer] = new_next[goal][buffer];
        }
        goal_state->active_index = 0;
        goal_state->build_index = 1;
        goal_state->stamp = 0u;
        goal_state->build.in_progress = false;
        goal_state->build.dist = NULL;
        goal_state->build.next = NULL;
        goal_state->build.neighbors = NULL;
        goal_state->build.goals = NULL;
        goal_state->build.goal_count = 0u;
        goal_state->build.eff_cost = NULL;
        goal_state->build.dirty_tiles = NULL;
        goal_state->build.dirty_count = 0u;
        heap_clear(&goal_state->build.heap);
    }

    g_field_state.tile_count = tile_count;
    return true;
}

void path_fields_shutdown_storage(void) {
    for (int goal = 0; goal < PATH_GOAL_COUNT; ++goal) {
        PathFieldGoalState *goal_state = &g_field_state.goals[goal];
        for (int buffer = 0; buffer < 2; ++buffer) {
            free(goal_state->dist[buffer]);
            free(goal_state->next[buffer]);
            goal_state->dist[buffer] = NULL;
            goal_state->next[buffer] = NULL;
        }
        heap_clear(&goal_state->build.heap);
        free(goal_state->build.heap.data);
        goal_state->build.heap.data = NULL;
        goal_state->build.heap.capacity = 0u;
        goal_state->build.heap.size = 0u;
        goal_state->build.in_progress = false;
        goal_state->build.dist = NULL;
        goal_state->build.next = NULL;
        goal_state->build.neighbors = NULL;
        goal_state->build.goals = NULL;
        goal_state->build.goal_count = 0u;
        goal_state->build.eff_cost = NULL;
        goal_state->build.dirty_tiles = NULL;
        goal_state->build.dirty_count = 0u;
        goal_state->stamp = 0u;
        goal_state->active_index = 0;
        goal_state->build_index = 1;
    }
    g_field_state.tile_count = 0u;
}

bool path_fields_start_build(PathGoal goal,
                             const HexWorld *world,
                             const int32_t *neighbors,
                             const TileId *goals,
                             size_t goal_count,
                             const float *eff_cost,
                             const TileId *dirty_tiles,
                             size_t dirty_count) {
    (void)world;
    PathFieldGoalState *goal_state = field_goal_state(goal);
    if (!goal_state) {
        return false;
    }
    if (!neighbors || !goals || goal_count == 0u) {
        LOG_WARN("path: cannot build field for goal %d without goals", (int)goal);
        return false;
    }
    if (g_field_state.tile_count == 0u) {
        return false;
    }

    PathFieldBuildState *build = &goal_state->build;
    build->neighbors = neighbors;
    build->goals = goals;
    build->goal_count = goal_count;
    build->eff_cost = eff_cost;
    build->dirty_tiles = dirty_tiles;
    build->dirty_count = dirty_count;
    build->dist = goal_state->dist[goal_state->build_index];
    build->next = goal_state->next[goal_state->build_index];
    if (!build->dist || !build->next) {
        return false;
    }

    size_t tile_count = g_field_state.tile_count;
    for (size_t i = 0; i < tile_count; ++i) {
        build->dist[i] = kInf;
    }
    memset(build->next, 0xFF, tile_count * sizeof(uint8_t));
    heap_clear(&build->heap);
    if (!heap_reserve(&build->heap, goal_count + dirty_count + 4u)) {
        LOG_ERROR("path: failed to reserve heap for goal %d", (int)goal);
        return false;
    }

    for (size_t i = 0; i < goal_count; ++i) {
        TileId goal_id = goals[i];
        if ((size_t)goal_id >= tile_count) {
            continue;
        }
        build->dist[goal_id] = 0.0f;
        build->next[goal_id] = 255u;
        if (!heap_push(&build->heap, goal_id, 0.0f)) {
            LOG_ERROR("path: failed to seed heap for goal %d", (int)goal);
            heap_clear(&build->heap);
            return false;
        }
    }

    if (dirty_tiles && dirty_count > 0u) {
        const float *active_dist = goal_state->dist[goal_state->active_index];
        const uint8_t *active_next = goal_state->next[goal_state->active_index];
        for (size_t i = 0; i < dirty_count; ++i) {
            TileId nid = dirty_tiles[i];
            if ((size_t)nid >= tile_count) {
                continue;
            }
            if (build->dist[nid] == 0.0f) {
                continue;
            }
            float seed_dist = (active_dist && (size_t)nid < tile_count) ? active_dist[nid] : kInf;
            uint8_t seed_next = (active_next && (size_t)nid < tile_count) ? active_next[nid] : 255u;
            if (seed_dist < kInf) {
                build->dist[nid] = seed_dist;
                build->next[nid] = seed_next;
                if (!heap_push(&build->heap, nid, seed_dist)) {
                    LOG_ERROR("path: failed to push dirty seed for goal %d", (int)goal);
                    path_fields_cancel_build(goal);
                    return false;
                }
            }
        }
    }

    if (build->heap.size == 0u) {
        LOG_WARN("path: goal %d build has no valid seeds", (int)goal);
        heap_clear(&build->heap);
        return false;
    }

    build->in_progress = true;
    return true;
}

bool path_fields_step(PathGoal goal,
                      double time_budget_ms,
                      size_t *out_relaxed,
                      double *out_elapsed_ms,
                      bool *out_finished) {
    if (out_relaxed) {
        *out_relaxed = 0u;
    }
    if (out_elapsed_ms) {
        *out_elapsed_ms = 0.0;
    }
    if (out_finished) {
        *out_finished = false;
    }

    PathFieldGoalState *goal_state = field_goal_state(goal);
    if (!goal_state) {
        return false;
    }

    PathFieldBuildState *build = &goal_state->build;
    if (!build->in_progress || !build->dist || !build->next || !build->neighbors) {
        return false;
    }

    size_t tile_count = g_field_state.tile_count;
    if (tile_count == 0u) {
        build->in_progress = false;
        return false;
    }

    double budget = time_budget_ms;
    if (budget < 0.0) {
        budget = 0.0;
    }
    bool check_time = (budget > 0.0);
    double start_ms = clock_now_ms();
    size_t relaxed = 0u;

    while (build->heap.size > 0u) {
        PathNode current;
        if (!heap_pop(&build->heap, &current)) {
            break;
        }
        if ((size_t)current.id >= tile_count) {
            continue;
        }
        if (current.dist > build->dist[current.id]) {
            continue;
        }
        const int32_t *nbrs = build->neighbors + (size_t)current.id * 6u;
        for (uint8_t dir = 0u; dir < 6u; ++dir) {
            int32_t neighbor = nbrs[dir];
            if (neighbor < 0) {
                continue;
            }
            size_t v = (size_t)neighbor;
            float tile_cost = 1.0f;
            if (build->eff_cost && v < tile_count) {
                tile_cost = build->eff_cost[v];
            }
            if (tile_cost < 0.0f) {
                tile_cost = 0.0f;
            }
            float alt = current.dist + tile_cost;
            if (alt < build->dist[v]) {
                build->dist[v] = alt;
                build->next[v] = opposite_direction(dir);
                if (!heap_push(&build->heap, (TileId)v, alt)) {
                    LOG_ERROR("path: failed to push node during goal %d build", (int)goal);
                    path_fields_cancel_build(goal);
                    return false;
                }
            }
        }
        ++relaxed;
        if (!check_time) {
            if (relaxed >= 1u) {
                break;
            }
        } else {
            double elapsed_now = clock_now_ms() - start_ms;
            if (elapsed_now >= budget) {
                break;
            }
        }
    }

    double elapsed_ms = clock_now_ms() - start_ms;
    if (out_relaxed) {
        *out_relaxed = relaxed;
    }
    if (out_elapsed_ms) {
        *out_elapsed_ms = elapsed_ms;
    }

    if (build->heap.size == 0u) {
        build->in_progress = false;
        goal_state->active_index = goal_state->build_index;
        goal_state->build_index = goal_state->active_index ^ 1;
        ++goal_state->stamp;
        if (goal_state->stamp == 0u) {
            goal_state->stamp = 1u;
        }
        build->dist = NULL;
        build->next = NULL;
        if (out_finished) {
            *out_finished = true;
        }
    }

    return true;
}

bool path_fields_is_building(PathGoal goal) {
    PathFieldGoalState *goal_state = field_goal_state(goal);
    if (!goal_state) {
        return false;
    }
    return goal_state->build.in_progress;
}

void path_fields_cancel_build(PathGoal goal) {
    PathFieldGoalState *goal_state = field_goal_state(goal);
    if (!goal_state) {
        return;
    }
    PathFieldBuildState *build = &goal_state->build;
    build->in_progress = false;
    build->dist = NULL;
    build->next = NULL;
    build->eff_cost = NULL;
    build->dirty_tiles = NULL;
    build->dirty_count = 0u;
    heap_clear(&build->heap);
}

const float *path_field_dist(PathGoal goal) {
    PathFieldGoalState *goal_state = field_goal_state(goal);
    if (!goal_state) {
        return NULL;
    }
    return goal_state->dist[goal_state->active_index];
}

const uint8_t *path_field_next(PathGoal goal) {
    PathFieldGoalState *goal_state = field_goal_state(goal);
    if (!goal_state) {
        return NULL;
    }
    return goal_state->next[goal_state->active_index];
}

uint32_t path_field_stamp(PathGoal goal) {
    PathFieldGoalState *goal_state = field_goal_state(goal);
    if (!goal_state) {
        return 0u;
    }
    return goal_state->stamp;
}

size_t path_field_tile_count(void) {
    return g_field_state.tile_count;
}
