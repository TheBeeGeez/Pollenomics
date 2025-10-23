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
    bool in_progress;
} PathFieldBuildState;

typedef struct PathFieldState {
    float *dist[2];
    uint8_t *next[2];
    size_t tile_count;
    int active_index;
    int build_index;
    uint32_t stamp;
    PathFieldBuildState entrance;
} PathFieldState;

static PathFieldState g_field_state = {0};

static const float kInf = FLT_MAX / 4.0f;

static double clock_now_ms(void) {
    return (double)clock() * (1000.0 / (double)CLOCKS_PER_SEC);
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
        heap->size = 0;
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
    while (index > 0) {
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
    if (!heap || heap->size == 0) {
        return false;
    }
    if (out_node) {
        *out_node = heap->data[0];
    }
    heap->size -= 1u;
    if (heap->size == 0) {
        return true;
    }
    heap->data[0] = heap->data[heap->size];
    size_t index = 0;
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
    if (tile_count == 0) {
        path_fields_shutdown_storage();
        return true;
    }
    bool size_changed = (g_field_state.tile_count != tile_count) || !g_field_state.dist[0] ||
                        !g_field_state.dist[1] || !g_field_state.next[0] || !g_field_state.next[1];
    if (size_changed) {
        float *dist0 = (float *)malloc(tile_count * sizeof(float));
        float *dist1 = (float *)malloc(tile_count * sizeof(float));
        uint8_t *next0 = (uint8_t *)malloc(tile_count * sizeof(uint8_t));
        uint8_t *next1 = (uint8_t *)malloc(tile_count * sizeof(uint8_t));
        if (!dist0 || !dist1 || !next0 || !next1) {
            free(dist0);
            free(dist1);
            free(next0);
            free(next1);
            LOG_ERROR("path: failed to allocate field storage (%zu tiles)", tile_count);
            return false;
        }
        free(g_field_state.dist[0]);
        free(g_field_state.dist[1]);
        free(g_field_state.next[0]);
        free(g_field_state.next[1]);
        g_field_state.dist[0] = dist0;
        g_field_state.dist[1] = dist1;
        g_field_state.next[0] = next0;
        g_field_state.next[1] = next1;
    }
    g_field_state.tile_count = tile_count;
    g_field_state.active_index = 0;
    g_field_state.build_index = 1;
    g_field_state.stamp = 0u;
    g_field_state.entrance.in_progress = false;
    g_field_state.entrance.dist = NULL;
    g_field_state.entrance.next = NULL;
    g_field_state.entrance.neighbors = NULL;
    g_field_state.entrance.goals = NULL;
    g_field_state.entrance.goal_count = 0u;
    heap_clear(&g_field_state.entrance.heap);
    return true;
}

void path_fields_shutdown_storage(void) {
    free(g_field_state.dist[0]);
    free(g_field_state.dist[1]);
    free(g_field_state.next[0]);
    free(g_field_state.next[1]);
    g_field_state.dist[0] = NULL;
    g_field_state.dist[1] = NULL;
    g_field_state.next[0] = NULL;
    g_field_state.next[1] = NULL;
    g_field_state.tile_count = 0u;
    g_field_state.active_index = 0;
    g_field_state.build_index = 1;
    g_field_state.stamp = 0u;
    g_field_state.entrance.in_progress = false;
    g_field_state.entrance.dist = NULL;
    g_field_state.entrance.next = NULL;
    g_field_state.entrance.neighbors = NULL;
    g_field_state.entrance.goals = NULL;
    g_field_state.entrance.goal_count = 0u;
    free(g_field_state.entrance.heap.data);
    g_field_state.entrance.heap.data = NULL;
    g_field_state.entrance.heap.size = 0u;
    g_field_state.entrance.heap.capacity = 0u;
}

bool path_fields_start_build(PathGoal goal,
                             const HexWorld *world,
                             const int32_t *neighbors,
                             const TileId *goals,
                             size_t goal_count) {
    (void)world;
    if (goal != PATH_GOAL_ENTRANCE) {
        return false;
    }
    if (!neighbors || !goals || goal_count == 0) {
        LOG_WARN("path: cannot build entrance field without goals");
        return false;
    }
    if (g_field_state.tile_count == 0u) {
        return false;
    }
    PathFieldBuildState *build = &g_field_state.entrance;
    build->neighbors = neighbors;
    build->goals = goals;
    build->goal_count = goal_count;
    build->dist = g_field_state.dist[g_field_state.build_index];
    build->next = g_field_state.next[g_field_state.build_index];
    if (!build->dist || !build->next) {
        return false;
    }
    size_t tile_count = g_field_state.tile_count;
    for (size_t i = 0; i < tile_count; ++i) {
        build->dist[i] = kInf;
    }
    memset(build->next, 0xFF, tile_count * sizeof(uint8_t));
    heap_clear(&build->heap);
    if (!heap_reserve(&build->heap, goal_count)) {
        LOG_ERROR("path: failed to reserve heap for entrance goals");
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
            LOG_ERROR("path: failed to seed entrance heap");
            heap_clear(&build->heap);
            return false;
        }
    }
    if (build->heap.size == 0u) {
        LOG_WARN("path: entrance build has no valid seeds");
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
    if (goal != PATH_GOAL_ENTRANCE) {
        return false;
    }
    PathFieldBuildState *build = &g_field_state.entrance;
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
        for (uint8_t dir = 0; dir < 6u; ++dir) {
            int32_t neighbor = nbrs[dir];
            if (neighbor < 0) {
                continue;
            }
            size_t v = (size_t)neighbor;
            float alt = current.dist + 1.0f;
            if (alt < build->dist[v]) {
                build->dist[v] = alt;
                build->next[v] = opposite_direction(dir);
                if (!heap_push(&build->heap, (TileId)v, alt)) {
                    LOG_ERROR("path: failed to push node during entrance build");
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
        g_field_state.active_index = g_field_state.build_index;
        g_field_state.build_index = g_field_state.active_index ^ 1;
        ++g_field_state.stamp;
        if (g_field_state.stamp == 0u) {
            g_field_state.stamp = 1u;
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
    if (goal != PATH_GOAL_ENTRANCE) {
        return false;
    }
    return g_field_state.entrance.in_progress;
}

void path_fields_cancel_build(PathGoal goal) {
    if (goal != PATH_GOAL_ENTRANCE) {
        return;
    }
    PathFieldBuildState *build = &g_field_state.entrance;
    build->in_progress = false;
    build->dist = NULL;
    build->next = NULL;
    heap_clear(&build->heap);
}

const float *path_field_dist(PathGoal goal) {
    if (goal != PATH_GOAL_ENTRANCE) {
        return NULL;
    }
    return g_field_state.dist[g_field_state.active_index];
}

const uint8_t *path_field_next(PathGoal goal) {
    if (goal != PATH_GOAL_ENTRANCE) {
        return NULL;
    }
    return g_field_state.next[g_field_state.active_index];
}

uint32_t path_field_stamp(PathGoal goal) {
    if (goal != PATH_GOAL_ENTRANCE) {
        return 0u;
    }
    return g_field_state.stamp;
}

size_t path_field_tile_count(void) {
    return g_field_state.tile_count;
}
