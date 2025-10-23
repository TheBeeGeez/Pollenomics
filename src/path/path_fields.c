#include "path/path_fields.h"

#include <float.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "path_internal.h"
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

static float *g_dist_entrance = NULL;
static uint8_t *g_next_entrance = NULL;
static uint32_t g_stamp_entrance = 0;
static size_t g_tile_count = 0;

static const float kInf = FLT_MAX / 4.0f;

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
    heap->data[0] = heap->data[heap->size - 1u];
    heap->size -= 1u;
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

bool path_fields_init_storage(size_t tile_count) {
    if (tile_count == 0) {
        path_fields_shutdown_storage();
        g_tile_count = 0;
        return true;
    }
    if (g_tile_count != tile_count) {
        float *new_dist = (float *)malloc(tile_count * sizeof(float));
        uint8_t *new_next = (uint8_t *)malloc(tile_count * sizeof(uint8_t));
        if (!new_dist || !new_next) {
            free(new_dist);
            free(new_next);
            LOG_ERROR("path: failed to allocate field storage (%zu tiles)", tile_count);
            return false;
        }
        free(g_dist_entrance);
        free(g_next_entrance);
        g_dist_entrance = new_dist;
        g_next_entrance = new_next;
        g_tile_count = tile_count;
    }
    return true;
}

void path_fields_shutdown_storage(void) {
    free(g_dist_entrance);
    free(g_next_entrance);
    g_dist_entrance = NULL;
    g_next_entrance = NULL;
    g_stamp_entrance = 0;
    g_tile_count = 0;
}

static uint8_t opposite_direction(uint8_t dir) {
    static const uint8_t kOpposite[6] = {3, 4, 5, 0, 1, 2};
    if (dir < 6) {
        return kOpposite[dir];
    }
    return 255u;
}

bool path_fields_build(PathGoal goal,
                       const HexWorld *world,
                       const int32_t *neighbors,
                       const TileId *goals,
                       size_t goal_count) {
    if (goal != PATH_GOAL_ENTRANCE) {
        return false;
    }
    (void)world;
    if (!g_dist_entrance || !g_next_entrance || !neighbors) {
        return false;
    }
    size_t tile_count = g_tile_count;
    if (tile_count == 0) {
        return true;
    }

    for (size_t i = 0; i < tile_count; ++i) {
        g_dist_entrance[i] = kInf;
        g_next_entrance[i] = 255u;
    }

    PathHeap heap = {0};
    if (!heap_reserve(&heap, goal_count > 0 ? goal_count : 1u)) {
        LOG_ERROR("path: failed to allocate heap");
        free(heap.data);
        return false;
    }

    for (size_t i = 0; i < goal_count; ++i) {
        TileId goal_id = goals[i];
        if (goal_id >= tile_count) {
            continue;
        }
        g_dist_entrance[goal_id] = 0.0f;
        g_next_entrance[goal_id] = 255u;
        if (!heap_push(&heap, goal_id, 0.0f)) {
            LOG_ERROR("path: failed to seed heap");
            free(heap.data);
            return false;
        }
    }

    const float edge_cost = 1.0f;

    PathNode current;
    while (heap_pop(&heap, &current)) {
        if (current.id >= tile_count) {
            continue;
        }
        if (current.dist > g_dist_entrance[current.id]) {
            continue;
        }
        const int32_t *nbrs = neighbors + current.id * 6u;
        for (uint8_t dir = 0; dir < 6u; ++dir) {
            int32_t neighbor = nbrs[dir];
            if (neighbor < 0) {
                continue;
            }
            size_t v = (size_t)neighbor;
            float alt = current.dist + edge_cost;
            if (alt < g_dist_entrance[v]) {
                g_dist_entrance[v] = alt;
                g_next_entrance[v] = opposite_direction(dir);
                if (!heap_push(&heap, (TileId)v, alt)) {
                    LOG_ERROR("path: failed to push node into heap");
                    free(heap.data);
                    return false;
                }
            }
        }
    }

    free(heap.data);
    ++g_stamp_entrance;
    if (g_stamp_entrance == 0) {
        g_stamp_entrance = 1;
    }
    return true;
}

const float *path_field_dist(PathGoal goal) {
    if (goal != PATH_GOAL_ENTRANCE) {
        return NULL;
    }
    return g_dist_entrance;
}

const uint8_t *path_field_next(PathGoal goal) {
    if (goal != PATH_GOAL_ENTRANCE) {
        return NULL;
    }
    return g_next_entrance;
}

uint32_t path_field_stamp(PathGoal goal) {
    if (goal != PATH_GOAL_ENTRANCE) {
        return 0u;
    }
    return g_stamp_entrance;
}

size_t path_field_tile_count(void) {
    return g_tile_count;
}

