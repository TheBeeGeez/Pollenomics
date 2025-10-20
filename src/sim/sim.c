#include "sim.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#include <malloc.h>
#endif

#include "util/log.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWO_PI (2.0f * (float)M_PI)

typedef struct SimState {
    size_t count;
    size_t capacity;
    uint64_t seed;
    float world_w;
    float world_h;
    float default_radius;
    float default_color[4];
    float min_speed;
    float max_speed;
    float jitter_rad_per_sec;
    float bounce_margin;
    float spawn_speed_mean;
    float spawn_speed_std;
    int spawn_mode;
    float *x;
    float *y;
    float *vx;
    float *vy;
    float *heading;
    float *radius;
    uint32_t *color_rgba;
    float *scratch_xy;
    uint64_t rng_state;
    double log_accum_sec;
    uint64_t log_bounce_count;
    uint64_t log_sample_count;
    double log_speed_sum;
    double log_speed_min;
    double log_speed_max;
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

static float clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
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

static float rand_uniform01(uint64_t *state) {
    uint64_t x = xorshift64(state);
    return (float)((x >> 11) * (1.0 / 9007199254740992.0));
}

static float rand_symmetric(uint64_t *state) {
    return rand_uniform01(state) * 2.0f - 1.0f;
}

static float rand_angle(uint64_t *state) {
    return rand_uniform01(state) * TWO_PI - (float)M_PI;
}

static float sample_gaussian(uint64_t *state, float mean, float stddev) {
    if (stddev <= 0.0f) {
        return mean;
    }
    float u1 = rand_uniform01(state);
    float u2 = rand_uniform01(state);
    u1 = u1 <= 1e-6f ? 1e-6f : u1;
    float mag = stddev * sqrtf(-2.0f * logf(u1));
    float z0 = mag * cosf(TWO_PI * u2);
    return mean + z0;
}

static float wrap_angle(float angle) {
    angle = fmodf(angle + (float)M_PI, TWO_PI);
    if (angle < 0.0f) {
        angle += TWO_PI;
    }
    return angle - (float)M_PI;
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

static void configure_from_params(SimState *state, const Params *params) {
    if (!state || !params) {
        return;
    }
    state->default_radius = params->bee_radius_px;
    for (int i = 0; i < 4; ++i) {
        state->default_color[i] = params->bee_color_rgba[i];
    }
    state->min_speed = params->motion_min_speed;
    state->max_speed = params->motion_max_speed;
    state->jitter_rad_per_sec = params->motion_jitter_deg_per_sec * (float)M_PI / 180.0f;
    state->bounce_margin = params->motion_bounce_margin;
    state->spawn_speed_mean = params->motion_spawn_speed_mean;
    state->spawn_speed_std = params->motion_spawn_speed_std;
    state->spawn_mode = params->motion_spawn_mode;
    state->seed = params->rng_seed;
}

static void reset_log_stats(SimState *state) {
    state->log_accum_sec = 0.0;
    state->log_bounce_count = 0;
    state->log_sample_count = 0;
    state->log_speed_sum = 0.0;
    state->log_speed_min = DBL_MAX;
    state->log_speed_max = 0.0;
}

static void fill_bees(SimState *state, const Params *params, uint64_t seed) {
    if (!state) {
        return;
    }

    if (params) {
        configure_from_params(state, params);
    }

    if (seed == 0) {
        seed = state->seed ? state->seed : UINT64_C(0xBEE);
    }
    state->seed = seed;
    state->rng_state = seed;

    const float bee_radius = state->default_radius;
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
    const float min_x_allowed = bee_radius + state->bounce_margin;
    const float max_x_allowed = state->world_w - bee_radius - state->bounce_margin;
    const float min_y_allowed = bee_radius + state->bounce_margin;
    const float max_y_allowed = state->world_h - bee_radius - state->bounce_margin;

    uint32_t palette[] = {
        0xD1491AFF,  // orange
        0xF1C40FFF,  // yellow
        0x1ABC9CFF,  // teal
        0x2980B9FF,  // blue
        0x8E44ADFF,  // purple
        0x2C3E50FF,  // slate
    };
    const size_t palette_count = sizeof(palette) / sizeof(palette[0]);

    uint64_t rng = state->rng_state;
    size_t palette_offset = (size_t)(xorshift64(&rng) % palette_count);

    for (size_t i = 0; i < state->count; ++i) {
        size_t col = i % cols;
        size_t row = i / cols;

        float base_x = origin_x + (float)col * spacing;
        float base_y = origin_y + (float)row * spacing;

        float jitter_x = rand_symmetric(&rng) * bee_radius * 0.25f;
        float jitter_y = rand_symmetric(&rng) * bee_radius * 0.25f;

        float x = base_x + jitter_x;
        float y = base_y + jitter_y;

        float clamped_min_x = min_x_allowed;
        float clamped_max_x = max_x_allowed;
        if (clamped_min_x > clamped_max_x) {
            float mid = state->world_w * 0.5f;
            clamped_min_x = clamped_max_x = mid;
        }
        float clamped_min_y = min_y_allowed;
        float clamped_max_y = max_y_allowed;
        if (clamped_min_y > clamped_max_y) {
            float mid = state->world_h * 0.5f;
            clamped_min_y = clamped_max_y = mid;
        }

        if (x < clamped_min_x) x = clamped_min_x;
        if (x > clamped_max_x) x = clamped_max_x;
        if (y < clamped_min_y) y = clamped_min_y;
        if (y > clamped_max_y) y = clamped_max_y;

        float heading = rand_angle(&rng);
        float speed = state->spawn_speed_mean;
        if (state->spawn_mode == SPAWN_VELOCITY_GAUSSIAN_DIR) {
            speed = sample_gaussian(&rng, state->spawn_speed_mean, state->spawn_speed_std);
        }
        speed = clampf(speed, state->min_speed, state->max_speed);

        state->x[i] = x;
        state->y[i] = y;
        state->heading[i] = heading;
        state->vx[i] = cosf(heading) * speed;
        state->vy[i] = sinf(heading) * speed;
        state->radius[i] = bee_radius;
        size_t palette_index = (palette_offset + i) % palette_count;
        state->color_rgba[i] = palette[palette_index];
    }

    if (state->count > 0) {
        state->color_rgba[0] = make_color(state->default_color[0],
                                          state->default_color[1],
                                          state->default_color[2],
                                          state->default_color[3]);
    }

    state->rng_state = rng;
    reset_log_stats(state);
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
    state->seed = params->rng_seed ? params->rng_seed : UINT64_C(0xBEE);
    state->world_w = params->world_width_px > 0.0f ? params->world_width_px
                                                   : (float)params->window_width_px;
    state->world_h = params->world_height_px > 0.0f ? params->world_height_px
                                                   : (float)params->window_height_px;
    configure_from_params(state, params);

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
    LOG_INFO("sim: initialized count=%zu capacity=%zu seed=0x%llx dt=%.5f max_speed=%.1f jitter=%.1fdeg/s",
             state->count,
             state->capacity,
             (unsigned long long)state->seed,
             params->sim_fixed_dt,
             params->motion_max_speed,
             params->motion_jitter_deg_per_sec);
    return true;
}

void sim_tick(SimState *state, float dt_sec) {
    if (!state || state->count == 0) {
        return;
    }
    if (dt_sec <= 0.0f) {
        update_scratch(state);
        return;
    }

    uint64_t rng = state->rng_state;
    const float jitter = state->jitter_rad_per_sec;
    const float min_speed = state->min_speed;
    const float max_speed = state->max_speed;
    const float bounce_margin = state->bounce_margin;
    const float world_w = state->world_w;
    const float world_h = state->world_h;
    const float damping = 0.98f;

    double speed_sum = 0.0;
    float speed_min_tick = FLT_MAX;
    float speed_max_tick = 0.0f;
    uint64_t bounce_counter = 0;

    for (size_t i = 0; i < state->count; ++i) {
        float heading = state->heading[i];
        float vx = state->vx[i];
        float vy = state->vy[i];

        if (jitter > 0.0f) {
            heading = wrap_angle(heading + jitter * dt_sec * rand_symmetric(&rng));
        }

        float speed = sqrtf(vx * vx + vy * vy);
        if (speed < 1e-4f) {
            heading = rand_angle(&rng);
            speed = 0.5f * (min_speed + max_speed);
        }
        speed = clampf(speed, min_speed, max_speed);
        vx = cosf(heading) * speed;
        vy = sinf(heading) * speed;

        float x = state->x[i] + vx * dt_sec;
        float y = state->y[i] + vy * dt_sec;
        float radius = state->radius[i];
        bool bounced = false;

        float min_x = radius + bounce_margin;
        float max_x = world_w - radius - bounce_margin;
        if (min_x > max_x) {
            float mid = world_w * 0.5f;
            min_x = max_x = mid;
        }
        if (x < min_x) {
            x = min_x;
            vx = -vx * damping;
            bounced = true;
        } else if (x > max_x) {
            x = max_x;
            vx = -vx * damping;
            bounced = true;
        }

        float min_y = radius + bounce_margin;
        float max_y = world_h - radius - bounce_margin;
        if (min_y > max_y) {
            float mid = world_h * 0.5f;
            min_y = max_y = mid;
        }
        if (y < min_y) {
            y = min_y;
            vy = -vy * damping;
            bounced = true;
        } else if (y > max_y) {
            y = max_y;
            vy = -vy * damping;
            bounced = true;
        }

        if (bounced) {
            ++bounce_counter;
            speed = sqrtf(vx * vx + vy * vy);
            if (speed < 1e-4f) {
                heading = rand_angle(&rng);
                speed = min_speed;
            } else {
                heading = wrap_angle(atan2f(vy, vx));
                speed = clampf(speed, min_speed, max_speed);
            }
            vx = cosf(heading) * speed;
            vy = sinf(heading) * speed;
        } else {
            heading = wrap_angle(atan2f(vy, vx));
        }

        state->x[i] = x;
        state->y[i] = y;
        state->vx[i] = vx;
        state->vy[i] = vy;
        state->heading[i] = heading;

        speed = sqrtf(vx * vx + vy * vy);
        if (speed < speed_min_tick) {
            speed_min_tick = speed;
        }
        if (speed > speed_max_tick) {
            speed_max_tick = speed;
        }
        speed_sum += speed;
    }

    state->rng_state = rng;
    update_scratch(state);

    state->log_accum_sec += dt_sec;
    state->log_bounce_count += bounce_counter;
    state->log_sample_count += state->count;
    state->log_speed_sum += speed_sum;
    if (state->count > 0) {
        if (state->log_speed_min > speed_min_tick) {
            state->log_speed_min = speed_min_tick;
        }
        if (state->log_speed_max < speed_max_tick) {
            state->log_speed_max = speed_max_tick;
        }
    }

    if (state->log_accum_sec >= 1.0) {
        double avg_speed = 0.0;
        if (state->log_sample_count > 0) {
            avg_speed = state->log_speed_sum / (double)state->log_sample_count;
        }
        double min_speed_log = state->log_speed_min == DBL_MAX ? 0.0 : state->log_speed_min;
        double max_speed_log = state->log_speed_max;
        float jitter_deg = state->jitter_rad_per_sec * 180.0f / (float)M_PI;
        LOG_INFO("sim: n=%zu dt=%.5f max_speed=%.1f jitter=%.1fdeg/s avg=%.1f min=%.1f max=%.1f bounces=%llu",
                 state->count,
                 dt_sec,
                 state->max_speed,
                 jitter_deg,
                 (float)avg_speed,
                 (float)min_speed_log,
                 (float)max_speed_log,
                 (unsigned long long)state->log_bounce_count);
        reset_log_stats(state);
    }
}

RenderView sim_build_view(const SimState *state) {
    RenderView view = (RenderView){0};
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
        seed = state->seed ? state->seed : UINT64_C(0xBEE);
    }
    fill_bees(state, NULL, seed);
    LOG_INFO("sim: reset seed=0x%llx", (unsigned long long)seed);
}

void sim_shutdown(SimState *state) {
    sim_release(state);
}
