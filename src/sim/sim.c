#include "sim.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifdef _MSC_VER
#include <malloc.h>
#endif

#include "util/log.h"

typedef struct SimState {
    size_t count;
    size_t capacity;
    uint64_t seed;
    float world_w;
    float world_h;
    float default_radius;
    float default_color[4];
    float *x;
    float *y;
    float *vx;
    float *vy;
    float *heading;
    float *radius;
    uint32_t *color_rgba;
    float *scratch_xy;
} SimState;

static void *alloc_aligned(size_t bytes) {
    if (bytes == 0) {
        return NULL;
    }
#if defined(_MSC_VER)
    void *ptr = _aligned_malloc(bytes, 16);
    if (ptr) {
        memset(ptr, 0, bytes);
    }
    return ptr;
#else
    void *ptr = NULL;
    if (posix_memalign(&ptr, 16, bytes) != 0) {
        return NULL;
    }
    memset(ptr, 0, bytes);
    return ptr;
#endif
}

static void free_aligned(void *ptr) {
#if defined(_MSC_VER)
    if (ptr) {
        _aligned_free(ptr);
    }
#else
    free(ptr);
#endif
}

static float clamp_positive(float value, float min_value) {
    return value < min_value ? min_value : value;
}

static uint32_t make_color(float r, float g, float b, float a) {
    uint32_t ri = (uint32_t)(r * 255.0f + 0.5f);
    uint32_t gi = (uint32_t)(g * 255.0f + 0.5f);
    uint32_t bi = (uint32_t)(b * 255.0f + 0.5f);
    uint32_t ai = (uint32_t)(a * 255.0f + 0.5f);
    if (ri > 255) ri = 255;
    if (gi > 255) gi = 255;
    if (bi > 255) bi = 255;
    if (ai > 255) ai = 255;
    return (ri << 24) | (gi << 16) | (bi << 8) | ai;
}

static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void update_scratch(SimState *state) {
    if (!state || !state->scratch_xy) {
        return;
    }
    for (size_t i = 0; i < state->count; ++i) {
        state->scratch_xy[2 * i + 0] = state->x[i];
        state->scratch_xy[2 * i + 1] = state->y[i];
    }
}

static void fill_bees(SimState *state, const Params *params, uint64_t seed) {
    if (!state) {
        return;
    }

    float bee_radius = state->default_radius;
    if (params) {
        bee_radius = params->bee_radius_px;
        state->default_radius = bee_radius;
        for (int i = 0; i < 4; ++i) {
            state->default_color[i] = params->bee_color_rgba[i];
        }
    }

    const float spacing = clamp_positive(bee_radius * 3.0f, bee_radius * 1.5f);
    size_t cols = (size_t)ceil(sqrt((double)state->capacity));
    if (cols == 0) {
        cols = 1;
    }
    size_t rows = (state->capacity + cols - 1u) / cols;

    const float grid_w = (float)(cols - 1) * spacing;
    const float grid_h = (float)(rows - 1) * spacing;
    const float origin_x = state->world_w * 0.5f - grid_w * 0.5f;
    const float origin_y = state->world_h * 0.5f - grid_h * 0.5f;

    uint32_t palette[] = {
        0xD1491AFF,  // orange
        0xF1C40FFF,  // yellow
        0x1ABC9CFF,  // teal
        0x2980B9FF,  // blue
        0x8E44ADFF,  // purple
        0x2C3E50FF,  // slate
    };
    const size_t palette_count = sizeof(palette) / sizeof(palette[0]);
    uint64_t rng = seed ? seed : UINT64_C(0xBEE);
    size_t palette_offset = (size_t)(xorshift64(&rng) % palette_count);

    for (size_t i = 0; i < state->count; ++i) {
        size_t col = i % cols;
        size_t row = i / cols;

        float jitter_x = 0.0f;
        float jitter_y = 0.0f;
        if (seed != 0) {
            jitter_x = (float)((int32_t)(xorshift64(&rng) & 0xFFFF) - 0x8000) / 0x8000;
            jitter_y = (float)((int32_t)(xorshift64(&rng) & 0xFFFF) - 0x8000) / 0x8000;
            jitter_x *= bee_radius * 0.25f;
            jitter_y *= bee_radius * 0.25f;
        }

        state->x[i] = origin_x + (float)col * spacing + jitter_x;
        state->y[i] = origin_y + (float)row * spacing + jitter_y;
        state->vx[i] = 0.0f;
        state->vy[i] = 0.0f;
        state->heading[i] = 0.0f;
        state->radius[i] = bee_radius;
        size_t palette_index = (palette_offset + i) % palette_count;
        state->color_rgba[i] = palette[palette_index];
    }

    if (state->count > 0) {
        const float *color = params ? params->bee_color_rgba : state->default_color;
        state->color_rgba[0] = make_color(color[0], color[1], color[2], color[3]);
    }

    update_scratch(state);
}

static void sim_release(SimState *state) {
    if (!state) {
        return;
    }
    free_aligned(state->x);
    free_aligned(state->y);
    free_aligned(state->vx);
    free_aligned(state->vy);
    free_aligned(state->heading);
    free_aligned(state->radius);
    free_aligned(state->color_rgba);
    free_aligned(state->scratch_xy);
    free(state);
}

bool sim_init(SimState **out_state, const Params *params) {
    if (!out_state || *out_state || !params) {
        LOG_ERROR("sim_init: invalid arguments");
        return false;
    }

    if (params->bee_count == 0) {
        LOG_ERROR("sim_init: bee_count must be > 0");
        return false;
    }

    SimState *state = (SimState *)calloc(1, sizeof(SimState));
    if (!state) {
        LOG_ERROR("sim_init: failed to allocate SimState");
        return false;
    }

    state->count = params->bee_count;
    state->capacity = params->bee_count;
    state->seed = params->rng_seed;
    state->world_w = params->world_width_px > 0.0f ? params->world_width_px
                                                   : (float)params->window_width_px;
    state->world_h = params->world_height_px > 0.0f ? params->world_height_px
                                                   : (float)params->window_height_px;
    state->default_radius = params->bee_radius_px;
    for (int i = 0; i < 4; ++i) {
        state->default_color[i] = params->bee_color_rgba[i];
    }

    size_t count = state->capacity;
    state->x = (float *)alloc_aligned(sizeof(float) * count);
    state->y = (float *)alloc_aligned(sizeof(float) * count);
    state->vx = (float *)alloc_aligned(sizeof(float) * count);
    state->vy = (float *)alloc_aligned(sizeof(float) * count);
    state->heading = (float *)alloc_aligned(sizeof(float) * count);
    state->radius = (float *)alloc_aligned(sizeof(float) * count);
    state->color_rgba = (uint32_t *)alloc_aligned(sizeof(uint32_t) * count);
    state->scratch_xy = (float *)alloc_aligned(sizeof(float) * count * 2u);

    if (!state->x || !state->y || !state->vx || !state->vy || !state->heading ||
        !state->radius || !state->color_rgba || !state->scratch_xy) {
        LOG_ERROR("sim_init: allocation failure for bee buffers");
        sim_release(state);
        return false;
    }

    fill_bees(state, params, state->seed);

    *out_state = state;
    LOG_INFO("sim: initialized count=%zu capacity=%zu seed=0x%llx",
             state->count,
             state->capacity,
             (unsigned long long)state->seed);
    return true;
}

void sim_tick(SimState *state, float dt_sec) {
    (void)dt_sec;
    if (!state) {
        return;
    }
    update_scratch(state);
}

RenderView sim_build_view(const SimState *state) {
    RenderView view = {0};
    if (!state || state->count == 0) {
        return view;
    }
    view.count = state->count;
    view.positions_xy = state->scratch_xy;
    view.radii_px = state->radius;
    view.color_rgba = state->color_rgba;
    return view;
}

void sim_reset(SimState *state, uint64_t seed) {
    if (!state) {
        return;
    }
    if (seed == 0) {
        seed = UINT64_C(0xBEE);
    }
    state->seed = seed;
    fill_bees(state, NULL, seed);
    LOG_INFO("sim: reset seed=0x%llx", (unsigned long long)seed);
}

void sim_shutdown(SimState *state) {
    sim_release(state);
}
