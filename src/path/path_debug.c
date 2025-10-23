#include "path/path_debug.h"

#include <stdlib.h>
#include <string.h>

#include "path/path_internal.h"
#include "util/log.h"

static float *g_overlay_lines_xy = NULL;
static uint32_t *g_overlay_line_rgba = NULL;
static size_t g_overlay_line_count = 0;

static float *g_frame_lines_xy = NULL;
static uint32_t *g_frame_line_rgba = NULL;
static size_t g_frame_line_capacity = 0;
static size_t g_frame_line_count = 0;

static bool g_debug_initialized = false;

bool path_debug_init(void) {
    g_debug_initialized = true;
    return true;
}

void path_debug_shutdown(void) {
    free(g_overlay_lines_xy);
    free(g_overlay_line_rgba);
    free(g_frame_lines_xy);
    free(g_frame_line_rgba);
    g_overlay_lines_xy = NULL;
    g_overlay_line_rgba = NULL;
    g_frame_lines_xy = NULL;
    g_frame_line_rgba = NULL;
    g_overlay_line_count = 0;
    g_frame_line_capacity = 0;
    g_frame_line_count = 0;
    g_debug_initialized = false;
}

void path_debug_reset_overlay(void) {
    g_overlay_line_count = 0;
}

static bool ensure_frame_capacity(size_t line_capacity) {
    if (line_capacity <= g_frame_line_capacity) {
        return true;
    }
    size_t new_capacity = g_frame_line_capacity ? g_frame_line_capacity : 128u;
    while (new_capacity < line_capacity) {
        new_capacity *= 2u;
    }
    float *new_xy = (float *)realloc(g_frame_lines_xy, new_capacity * 4u * sizeof(float));
    uint32_t *new_rgba = (uint32_t *)realloc(g_frame_line_rgba, new_capacity * sizeof(uint32_t));
    if (!new_xy || !new_rgba) {
        free(new_xy);
        free(new_rgba);
        return false;
    }
    g_frame_lines_xy = new_xy;
    g_frame_line_rgba = new_rgba;
    g_frame_line_capacity = new_capacity;
    return true;
}

bool path_debug_build_overlay(const HexWorld *world,
                              PathGoal goal,
                              const uint8_t *next,
                              size_t tile_count) {
    if (!g_debug_initialized) {
        if (!path_debug_init()) {
            return false;
        }
    }
    if (!world || !next || goal != PATH_GOAL_ENTRANCE) {
        g_overlay_line_count = 0;
        return false;
    }
    const float *centers = hex_world_centers_xy(world);
    float cell_radius = hex_world_cell_radius(world);
    if (!centers || cell_radius <= 0.0f) {
        g_overlay_line_count = 0;
        return false;
    }

    size_t arrow_count = 0;
    for (size_t i = 0; i < tile_count; ++i) {
        if (next[i] < 6u) {
            ++arrow_count;
        }
    }

    if (arrow_count == 0) {
        g_overlay_line_count = 0;
        return true;
    }

    float *new_xy = (float *)realloc(g_overlay_lines_xy, arrow_count * 4u * sizeof(float));
    uint32_t *new_rgba = (uint32_t *)realloc(g_overlay_line_rgba, arrow_count * sizeof(uint32_t));
    if (!new_xy || !new_rgba) {
        free(new_xy);
        free(new_rgba);
        LOG_WARN("path_debug: failed to allocate overlay buffer (%zu arrows)", arrow_count);
        g_overlay_line_count = 0;
        return false;
    }
    g_overlay_lines_xy = new_xy;
    g_overlay_line_rgba = new_rgba;

    const uint32_t arrow_color = 0x33FF66FFu;
    const float arrow_scale = cell_radius * 0.6f;

    size_t arrow_index = 0;
    for (size_t i = 0; i < tile_count; ++i) {
        uint8_t dir = next[i];
        if (dir >= 6u) {
            continue;
        }
        const float *dir_world = path_core_direction_world(dir);
        float cx = centers[i * 2u + 0u];
        float cy = centers[i * 2u + 1u];
        float dx = dir_world ? dir_world[0] : 0.0f;
        float dy = dir_world ? dir_world[1] : 0.0f;
        float ex = cx + dx * arrow_scale;
        float ey = cy + dy * arrow_scale;
        size_t base = arrow_index * 4u;
        g_overlay_lines_xy[base + 0u] = cx;
        g_overlay_lines_xy[base + 1u] = cy;
        g_overlay_lines_xy[base + 2u] = ex;
        g_overlay_lines_xy[base + 3u] = ey;
        g_overlay_line_rgba[arrow_index] = arrow_color;
        ++arrow_index;
    }

    g_overlay_line_count = arrow_index;
    return true;
}

void path_debug_begin_frame(void) {
    if (!ensure_frame_capacity(g_overlay_line_count)) {
        g_frame_line_count = 0;
        return;
    }
    if (g_overlay_line_count > 0 && g_overlay_lines_xy && g_overlay_line_rgba) {
        memcpy(g_frame_lines_xy, g_overlay_lines_xy, g_overlay_line_count * 4u * sizeof(float));
        memcpy(g_frame_line_rgba, g_overlay_line_rgba, g_overlay_line_count * sizeof(uint32_t));
    }
    g_frame_line_count = g_overlay_line_count;
}

bool path_debug_add_line(float x0, float y0, float x1, float y1, uint32_t color_rgba) {
    if (!ensure_frame_capacity(g_frame_line_count + 1u)) {
        return false;
    }
    size_t base = g_frame_line_count * 4u;
    g_frame_lines_xy[base + 0u] = x0;
    g_frame_lines_xy[base + 1u] = y0;
    g_frame_lines_xy[base + 2u] = x1;
    g_frame_lines_xy[base + 3u] = y1;
    g_frame_line_rgba[g_frame_line_count] = color_rgba;
    g_frame_line_count += 1u;
    return true;
}

const float *path_debug_lines_xy(void) {
    return (g_frame_line_count > 0) ? g_frame_lines_xy : NULL;
}

const uint32_t *path_debug_lines_rgba(void) {
    return (g_frame_line_count > 0) ? g_frame_line_rgba : NULL;
}

size_t path_debug_line_count(void) {
    return g_frame_line_count;
}

