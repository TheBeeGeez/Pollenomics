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
#define SIM_MAX_FLOWER_PATCHES 8

typedef struct HiveSegment {
    float ax;
    float ay;
    float bx;
    float by;
    float nx;
    float ny;
} HiveSegment;

typedef struct FlowerPatch {
    float x;
    float y;
    float radius;
    float quality;
    float stock;
    float capacity;
    float replenish_rate;
    float initial_stock;
} FlowerPatch;

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
    float *age_days;
    float *t_state;
    float *energy;
    float *load_nectar;
    float *target_pos_x;
    float *target_pos_y;
    int32_t *target_id;
    int16_t *topic_id;
    uint8_t *topic_confidence;
    uint8_t *role;
    uint8_t *mode;
    uint8_t *intent;
    float *capacity_uL;
    float *harvest_rate_uLps;
    uint8_t *inside_hive_flag;
    uint64_t rng_state;
    double log_accum_sec;
    uint64_t log_bounce_count;
    uint64_t log_sample_count;
    double log_speed_sum;
    double log_speed_min;
    double log_speed_max;

    // Hive collision data.
    int hive_enabled;
    float hive_rect_x;
    float hive_rect_y;
    float hive_rect_w;
    float hive_rect_h;
    int hive_entrance_side;
    float hive_entrance_t;
    float hive_entrance_width;
    float hive_restitution;
    float hive_tangent_damp;
    int hive_max_iters;
    float hive_safety_margin;
    HiveSegment hive_segments[8];
    size_t hive_segment_count;
    size_t patch_count;
    FlowerPatch patches[SIM_MAX_FLOWER_PATCHES];
    float bee_capacity_uL;
    float bee_harvest_rate_uLps;
    float bee_unload_rate_uLps;
    float bee_rest_recovery_per_s;
    float bee_speed_mps;
    float bee_seek_accel;
    float bee_arrive_tol_world;
    float patch_positions_xy[SIM_MAX_FLOWER_PATCHES * 2];
    float patch_radii_px[SIM_MAX_FLOWER_PATCHES];
    uint32_t patch_fill_rgba[SIM_MAX_FLOWER_PATCHES];
    float patch_ring_radii_px[SIM_MAX_FLOWER_PATCHES];
    uint32_t patch_ring_rgba[SIM_MAX_FLOWER_PATCHES];
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

static uint32_t bee_mode_color(uint8_t mode) {
    switch (mode) {
        case BEE_MODE_OUTBOUND: return make_color(0.25f, 0.45f, 0.85f, 1.0f);
        case BEE_MODE_FORAGING: return make_color(0.92f, 0.84f, 0.22f, 1.0f);
        case BEE_MODE_RETURNING: return make_color(0.95f, 0.55f, 0.18f, 1.0f);
        case BEE_MODE_ENTERING: return make_color(0.30f, 0.80f, 0.85f, 1.0f);
        case BEE_MODE_UNLOADING: return make_color(0.32f, 0.68f, 0.28f, 1.0f);
        case BEE_MODE_IDLE:
        default:
            return make_color(0.62f, 0.62f, 0.64f, 1.0f);
    }
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

static void hive_clear_segments(SimState *state) {
    if (!state) {
        return;
    }
    state->hive_segment_count = 0;
}

static void hive_add_segment(SimState *state,
                             float ax,
                             float ay,
                             float bx,
                             float by,
                             float nx,
                             float ny) {
    if (!state) {
        return;
    }
    float dx = bx - ax;
    float dy = by - ay;
    float len_sq = dx * dx + dy * dy;
    if (len_sq < 1e-6f) {
        return;
    }
    if (state->hive_segment_count >= sizeof(state->hive_segments) / sizeof(state->hive_segments[0])) {
        return;
    }
    float n_len = sqrtf(nx * nx + ny * ny);
    if (n_len > 0.0f) {
        nx /= n_len;
        ny /= n_len;
    } else {
        nx = 0.0f;
        ny = -1.0f;
    }
    HiveSegment *seg = &state->hive_segments[state->hive_segment_count++];
    seg->ax = ax;
    seg->ay = ay;
    seg->bx = bx;
    seg->by = by;
    seg->nx = nx;
    seg->ny = ny;
}

static void sim_build_hive_segments(SimState *state) {
    hive_clear_segments(state);
    if (!state) {
        return;
    }
    if (state->hive_rect_w <= 0.0f || state->hive_rect_h <= 0.0f) {
        state->hive_enabled = 0;
        return;
    }
    state->hive_enabled = 1;

    float x = state->hive_rect_x;
    float y = state->hive_rect_y;
    float w = state->hive_rect_w;
    float h = state->hive_rect_h;
    int side = state->hive_entrance_side;

    float gap_half = state->hive_entrance_width * 0.5f;
    float gap_min = 0.0f;
    float gap_max = 0.0f;

    if (side == 0 || side == 1) {
        float side_len = w;
        float gap_center = x + state->hive_entrance_t * side_len;
        gap_min = fmaxf(x, gap_center - gap_half);
        gap_max = fminf(x + w, gap_center + gap_half);
    } else {
        float side_len = h;
        float gap_center = y + state->hive_entrance_t * side_len;
        gap_min = fmaxf(y, gap_center - gap_half);
        gap_max = fminf(y + h, gap_center + gap_half);
    }

    // Top side (0).
    if (side == 0) {
        if (gap_min > x) {
            hive_add_segment(state, x, y, gap_min, y, 0.0f, -1.0f);
        }
        if (gap_max < x + w) {
            hive_add_segment(state, gap_max, y, x + w, y, 0.0f, -1.0f);
        }
    } else {
        hive_add_segment(state, x, y, x + w, y, 0.0f, -1.0f);
    }

    // Bottom side (1).
    if (side == 1) {
        float yb = y + h;
        if (gap_min > x) {
            hive_add_segment(state, x, yb, gap_min, yb, 0.0f, 1.0f);
        }
        if (gap_max < x + w) {
            hive_add_segment(state, gap_max, yb, x + w, yb, 0.0f, 1.0f);
        }
    } else {
        hive_add_segment(state, x, y + h, x + w, y + h, 0.0f, 1.0f);
    }

    // Left side (2).
    if (side == 2) {
        if (gap_min > y) {
            hive_add_segment(state, x, y, x, gap_min, -1.0f, 0.0f);
        }
        if (gap_max < y + h) {
            hive_add_segment(state, x, gap_max, x, y + h, -1.0f, 0.0f);
        }
    } else {
        hive_add_segment(state, x, y, x, y + h, -1.0f, 0.0f);
    }

    // Right side (3).
    if (side == 3) {
        float xr = x + w;
        if (gap_min > y) {
            hive_add_segment(state, xr, y, xr, gap_min, 1.0f, 0.0f);
        }
        if (gap_max < y + h) {
            hive_add_segment(state, xr, gap_max, xr, y + h, 1.0f, 0.0f);
        }
    } else {
        hive_add_segment(state, x + w, y, x + w, y + h, 1.0f, 0.0f);
    }
}

static int hive_resolve_segment(const SimState *state,
                                const HiveSegment *seg,
                                float radius,
                                float *px,
                                float *py,
                                float *vx,
                                float *vy) {
    float ax = seg->ax;
    float ay = seg->ay;
    float bx = seg->bx;
    float by = seg->by;

    float abx = bx - ax;
    float aby = by - ay;
    float ab_len_sq = abx * abx + aby * aby;
    if (ab_len_sq <= 1e-8f) {
        return 0;
    }

    float apx = *px - ax;
    float apy = *py - ay;
    float t = (apx * abx + apy * aby) / ab_len_sq;
    t = clampf(t, 0.0f, 1.0f);
    float cx = ax + abx * t;
    float cy = ay + aby * t;

    float rx = *px - cx;
    float ry = *py - cy;
    float dist_sq = rx * rx + ry * ry;
    float radius_sq = radius * radius;

    if (dist_sq >= radius_sq) {
        return 0;
    }

    float dist = sqrtf(dist_sq);
    float nx;
    float ny;
    if (dist > 1e-6f) {
        nx = rx / dist;
        ny = ry / dist;
    } else {
        nx = seg->nx;
        ny = seg->ny;
        float n_len = sqrtf(nx * nx + ny * ny);
        if (n_len <= 1e-6f) {
            nx = 0.0f;
            ny = -1.0f;
        } else {
            nx /= n_len;
            ny /= n_len;
        }
    }

    float penetration = radius - dist;
    if (penetration < 0.0f) {
        return 0;
    }
    penetration += state->hive_safety_margin;
    *px += nx * penetration;
    *py += ny * penetration;

    float v_normal = (*vx) * nx + (*vy) * ny;
    float vt_x = *vx - v_normal * nx;
    float vt_y = *vy - v_normal * ny;

    float new_vn = -state->hive_restitution * v_normal;
    float new_vt_x = state->hive_tangent_damp * vt_x;
    float new_vt_y = state->hive_tangent_damp * vt_y;

    *vx = new_vn * nx + new_vt_x;
    *vy = new_vn * ny + new_vt_y;
    return 1;
}

static void hive_resolve_disc(const SimState *state,
                              float radius,
                              float *x,
                              float *y,
                              float *vx,
                              float *vy) {
    if (!state || !state->hive_enabled || state->hive_segment_count == 0) {
        return;
    }
    int max_iters = state->hive_max_iters > 0 ? state->hive_max_iters : 1;
    for (int iter = 0; iter < max_iters; ++iter) {
        int collided = 0;
        for (size_t si = 0; si < state->hive_segment_count; ++si) {
            collided |= hive_resolve_segment(state, &state->hive_segments[si], radius, x, y, vx, vy);
        }
        if (!collided) {
            break;
        }
    }
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
    state->hive_rect_x = params->hive.rect_x;
    state->hive_rect_y = params->hive.rect_y;
    state->hive_rect_w = params->hive.rect_w;
    state->hive_rect_h = params->hive.rect_h;
    state->hive_entrance_side = params->hive.entrance_side;
    state->hive_entrance_t = params->hive.entrance_t;
    state->hive_entrance_width = params->hive.entrance_width;
    state->hive_restitution = params->hive.restitution;
    state->hive_tangent_damp = params->hive.tangent_damp;
    state->hive_max_iters = params->hive.max_resolve_iters;
    state->hive_safety_margin = params->hive.safety_margin;
    state->bee_capacity_uL = params->bee.capacity_uL;
    state->bee_harvest_rate_uLps = params->bee.harvest_rate_uLps;
    state->bee_unload_rate_uLps = params->bee.unload_rate_uLps;
    state->bee_rest_recovery_per_s = params->bee.rest_recovery_per_s;
    state->bee_speed_mps = params->bee.speed_mps;
    state->bee_seek_accel = params->bee.seek_accel;
    state->bee_arrive_tol_world = params->bee.arrive_tol_world;
    sim_build_hive_segments(state);

    if (state->capacity_uL && state->harvest_rate_uLps) {
        for (size_t i = 0; i < state->count; ++i) {
            state->capacity_uL[i] = state->bee_capacity_uL;
            state->harvest_rate_uLps[i] = state->bee_harvest_rate_uLps;
            if (state->load_nectar && state->load_nectar[i] > state->capacity_uL[i]) {
                state->load_nectar[i] = state->capacity_uL[i];
            }
        }
    }
}

static void reset_log_stats(SimState *state) {
    state->log_accum_sec = 0.0;
    state->log_bounce_count = 0;
    state->log_sample_count = 0;
    state->log_speed_sum = 0.0;
    state->log_speed_min = DBL_MAX;
    state->log_speed_max = 0.0;
}

static bool patch_location_valid(const SimState *state,
                                 float x,
                                 float y,
                                 float radius,
                                 size_t existing_count) {
    if (!state) {
        return false;
    }
    const float edge_margin = radius + state->default_radius * 4.0f;
    if (x - radius < edge_margin || x + radius > state->world_w - edge_margin ||
        y - radius < edge_margin || y + radius > state->world_h - edge_margin) {
        return false;
    }
    if (state->hive_rect_w > 0.0f && state->hive_rect_h > 0.0f) {
        float hx0 = state->hive_rect_x - edge_margin;
        float hy0 = state->hive_rect_y - edge_margin;
        float hx1 = state->hive_rect_x + state->hive_rect_w + edge_margin;
        float hy1 = state->hive_rect_y + state->hive_rect_h + edge_margin;
        if (x >= hx0 && x <= hx1 && y >= hy0 && y <= hy1) {
            return false;
        }
    }
    for (size_t i = 0; i < existing_count; ++i) {
        const FlowerPatch *patch = &state->patches[i];
        float dx = patch->x - x;
        float dy = patch->y - y;
        float dist_sq = dx * dx + dy * dy;
        float min_sep = patch->radius + radius + state->default_radius * 3.0f;
        if (dist_sq < (min_sep * min_sep)) {
            return false;
        }
    }
    return true;
}

static void generate_flower_patches(SimState *state, uint64_t *rng_state) {
    if (!state) {
        return;
    }
    uint64_t scratch_rng = rng_state ? *rng_state : state->rng_state;
    const size_t min_patches = 3;
    const size_t max_patches = SIM_MAX_FLOWER_PATCHES;
    size_t count = min_patches;
    if (max_patches > min_patches) {
        float roll = rand_uniform01(&scratch_rng);
        size_t span = max_patches - min_patches + 1;
        count = min_patches + (size_t)floorf(roll * (float)span);
        if (count > max_patches) {
            count = max_patches;
        }
        if (count < min_patches) {
            count = min_patches;
        }
    }

    state->patch_count = 0;
    for (size_t i = 0; i < count; ++i) {
        const float radius_min = 60.0f;
        const float radius_max = 140.0f;
        float radius = radius_min + (radius_max - radius_min) * rand_uniform01(&scratch_rng);

        float px = state->world_w * 0.5f;
        float py = state->world_h * 0.5f;
        bool placed = false;
        for (int attempt = 0; attempt < 64; ++attempt) {
            float rx = rand_uniform01(&scratch_rng) * state->world_w;
            float ry = rand_uniform01(&scratch_rng) * state->world_h;
            if (patch_location_valid(state, rx, ry, radius, state->patch_count)) {
                px = rx;
                py = ry;
                placed = true;
                break;
            }
        }
        if (!placed) {
            float angle = TWO_PI * rand_uniform01(&scratch_rng);
            float dist = fminf(state->world_w, state->world_h) * (0.35f + 0.15f * rand_uniform01(&scratch_rng));
            px = state->world_w * 0.5f + cosf(angle) * dist;
            py = state->world_h * 0.5f + sinf(angle) * dist;
            px = clampf(px, radius, state->world_w - radius);
            py = clampf(py, radius, state->world_h - radius);
        }

        float quality = 0.55f + 0.45f * rand_uniform01(&scratch_rng);
        float capacity = radius * quality * 12.0f;
        float initial = capacity * (0.65f + 0.25f * rand_uniform01(&scratch_rng));
        float replenish = quality * 6.0f;

        FlowerPatch patch = {
            .x = px,
            .y = py,
            .radius = radius,
            .quality = quality,
            .stock = initial,
            .capacity = capacity,
            .replenish_rate = replenish,
            .initial_stock = initial,
        };
        state->patches[state->patch_count++] = patch;
    }

    if (rng_state) {
        *rng_state = scratch_rng;
    } else {
        state->rng_state = scratch_rng;
    }
}

static void compute_hive_points(const SimState *state,
                                float *entrance_x,
                                float *entrance_y,
                                float *unload_x,
                                float *unload_y) {
    if (!state) {
        if (entrance_x) *entrance_x = 0.0f;
        if (entrance_y) *entrance_y = 0.0f;
        if (unload_x) *unload_x = 0.0f;
        if (unload_y) *unload_y = 0.0f;
        return;
    }
    const float hx = state->hive_rect_x;
    const float hy = state->hive_rect_y;
    const float hw = state->hive_rect_w;
    const float hh = state->hive_rect_h;
    const float t = clampf(state->hive_entrance_t, 0.0f, 1.0f);
    float ex = hx + hw * 0.5f;
    float ey = hy + hh * 0.5f;
    switch (state->hive_entrance_side) {
        case 0:  // top
            ex = hx + hw * t;
            ey = hy;
            break;
        case 1:  // bottom
            ex = hx + hw * t;
            ey = hy + hh;
            break;
        case 2:  // left
            ex = hx;
            ey = hy + hh * t;
            break;
        case 3:  // right
            ex = hx + hw;
            ey = hy + hh * t;
            break;
        default:
            break;
    }
    if (entrance_x) *entrance_x = ex;
    if (entrance_y) *entrance_y = ey;

    float unload_px = hx + hw * 0.5f;
    float unload_py = hy + hh * 0.6f;
    if (unload_x) *unload_x = unload_px;
    if (unload_y) *unload_y = unload_py;
}

static FlowerPatch *sim_get_patch(SimState *state, int32_t patch_id) {
    if (!state || patch_id < 0) {
        return NULL;
    }
    if ((size_t)patch_id >= state->patch_count) {
        return NULL;
    }
    return &state->patches[patch_id];
}

static const FlowerPatch *sim_get_patch_const(const SimState *state, int32_t patch_id) {
    if (!state || patch_id < 0) {
        return NULL;
    }
    if ((size_t)patch_id >= state->patch_count) {
        return NULL;
    }
    return &state->patches[patch_id];
}

static void replenish_patches(SimState *state, float dt_sec) {
    if (!state || dt_sec <= 0.0f) {
        return;
    }
    for (size_t i = 0; i < state->patch_count; ++i) {
        FlowerPatch *patch = &state->patches[i];
        patch->stock += patch->replenish_rate * dt_sec;
        if (patch->stock > patch->capacity) {
            patch->stock = patch->capacity;
        }
    }
}

static void sample_patch_point(const FlowerPatch *patch, uint64_t *rng, float *out_x, float *out_y) {
    if (!patch) {
        if (out_x) *out_x = 0.0f;
        if (out_y) *out_y = 0.0f;
        return;
    }
    float radius = patch->radius;
    float angle = rand_uniform01(rng) * TWO_PI;
    float r = radius * sqrtf(rand_uniform01(rng));
    if (out_x) *out_x = patch->x + cosf(angle) * r;
    if (out_y) *out_y = patch->y + sinf(angle) * r;
}

static int32_t choose_flower_patch(const SimState *state,
                                   float from_x,
                                   float from_y,
                                   uint64_t *rng) {
    if (!state || state->patch_count == 0) {
        return -1;
    }
    int32_t best_index = -1;
    float best_score = -FLT_MAX;
    for (size_t i = 0; i < state->patch_count; ++i) {
        const FlowerPatch *patch = &state->patches[i];
        if (patch->stock <= 0.5f) {
            continue;
        }
        float dx = patch->x - from_x;
        float dy = patch->y - from_y;
        float distance = sqrtf(dx * dx + dy * dy) + 1.0f;
        float stock_factor = patch->stock / fmaxf(1.0f, patch->capacity);
        float quality = patch->quality;
        float score = (stock_factor * quality) / distance;
        if (rng) {
            float jitter = 1.0f + 0.1f * (rand_uniform01(rng) - 0.5f);
            score *= jitter;
        }
        if (score > best_score) {
            best_score = score;
            best_index = (int32_t)i;
        }
    }
    if (best_index < 0 && state->patch_count > 0) {
        best_index = (int32_t)(rand_uniform01(rng) * (float)state->patch_count);
        if ((size_t)best_index >= state->patch_count) {
            best_index = (int32_t)(state->patch_count - 1);
        }
    }
    return best_index;
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

    float entrance_x = state->world_w * 0.5f;
    float entrance_y = state->world_h * 0.5f;
    float unload_x = entrance_x;
    float unload_y = entrance_y;
    compute_hive_points(state, &entrance_x, &entrance_y, &unload_x, &unload_y);

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


    uint64_t rng = state->rng_state;

    generate_flower_patches(state, &rng);

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

        state->x[i] = x;
        state->y[i] = y;
        state->heading[i] = heading;
        state->vx[i] = 0.0f;
        state->vy[i] = 0.0f;
        state->radius[i] = bee_radius;

        float age_days = rand_uniform01(&rng) * 25.0f;
        state->age_days[i] = age_days;
        state->t_state[i] = 0.0f;
        state->energy[i] = 1.0f;
        state->load_nectar[i] = 0.0f;
        state->target_pos_x[i] = unload_x;
        state->target_pos_y[i] = unload_y;
        state->target_id[i] = -1;
        state->topic_id[i] = -1;
        state->topic_confidence[i] = 0;
        state->capacity_uL[i] = state->bee_capacity_uL;
        state->harvest_rate_uLps[i] = state->bee_harvest_rate_uLps;

        BeeRole role = bee_pick_role(age_days, &rng);
        state->role[i] = (uint8_t)role;

        state->mode[i] = (uint8_t)BEE_MODE_IDLE;
        state->intent[i] = (uint8_t)BEE_INTENT_REST;
        state->color_rgba[i] = bee_mode_color(state->mode[i]);

        bool inside = state->hive_enabled &&
                      x >= state->hive_rect_x &&
                      x <= state->hive_rect_x + state->hive_rect_w &&
                      y >= state->hive_rect_y &&
                      y <= state->hive_rect_y + state->hive_rect_h;
        state->inside_hive_flag[i] = inside ? 1u : 0u;
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
    free_aligned(state->age_days);
    free_aligned(state->t_state);
    free_aligned(state->energy);
    free_aligned(state->load_nectar);
    free_aligned(state->target_pos_x);
    free_aligned(state->target_pos_y);
    free_aligned(state->target_id);
    free_aligned(state->topic_id);
    free_aligned(state->topic_confidence);
    free_aligned(state->role);
    free_aligned(state->mode);
    free_aligned(state->intent);
    free_aligned(state->capacity_uL);
    free_aligned(state->harvest_rate_uLps);
    free_aligned(state->inside_hive_flag);
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
    state->age_days = (float *)alloc_aligned(sizeof(float) * count);
    state->t_state = (float *)alloc_aligned(sizeof(float) * count);
    state->energy = (float *)alloc_aligned(sizeof(float) * count);
    state->load_nectar = (float *)alloc_aligned(sizeof(float) * count);
    state->target_pos_x = (float *)alloc_aligned(sizeof(float) * count);
    state->target_pos_y = (float *)alloc_aligned(sizeof(float) * count);
    state->target_id = (int32_t *)alloc_aligned(sizeof(int32_t) * count);
    state->topic_id = (int16_t *)alloc_aligned(sizeof(int16_t) * count);
    state->topic_confidence = (uint8_t *)alloc_aligned(sizeof(uint8_t) * count);
    state->role = (uint8_t *)alloc_aligned(sizeof(uint8_t) * count);
    state->mode = (uint8_t *)alloc_aligned(sizeof(uint8_t) * count);
    state->intent = (uint8_t *)alloc_aligned(sizeof(uint8_t) * count);
    state->capacity_uL = (float *)alloc_aligned(sizeof(float) * count);
    state->harvest_rate_uLps = (float *)alloc_aligned(sizeof(float) * count);
    state->inside_hive_flag = (uint8_t *)alloc_aligned(sizeof(uint8_t) * count);

    if (!state->x || !state->y || !state->vx || !state->vy || !state->heading ||
        !state->radius || !state->color_rgba || !state->scratch_xy ||
        !state->age_days || !state->t_state || !state->energy || !state->load_nectar ||
        !state->target_pos_x || !state->target_pos_y || !state->target_id ||
        !state->topic_id || !state->topic_confidence || !state->role ||
        !state->mode || !state->intent || !state->capacity_uL || !state->harvest_rate_uLps ||
        !state->inside_hive_flag) {
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

    replenish_patches(state, dt_sec);

    uint64_t rng = state->rng_state;
    const float world_w = state->world_w;
    const float world_h = state->world_h;
    const float bounce_margin = state->bounce_margin;
    const float base_speed = state->bee_speed_mps > 0.0f ? state->bee_speed_mps : state->max_speed;
    const float max_speed = base_speed > 0.0f ? base_speed : state->max_speed;
    const float seek_accel = state->bee_seek_accel > 0.0f ? state->bee_seek_accel : state->max_speed * 2.0f;
    float arrive_tol = state->bee_arrive_tol_world > 0.0f ? state->bee_arrive_tol_world : state->default_radius * 2.0f;

    float entrance_x = world_w * 0.5f;
    float entrance_y = world_h * 0.5f;
    float unload_x = entrance_x;
    float unload_y = entrance_y;
    compute_hive_points(state, &entrance_x, &entrance_y, &unload_x, &unload_y);

    double speed_sum = 0.0;
    float speed_min_tick = FLT_MAX;
    float speed_max_tick = 0.0f;
    uint64_t bounce_counter = 0;
    bool any_patch_available = false;
    for (size_t pi = 0; pi < state->patch_count; ++pi) {
        if (state->patches[pi].stock > 0.5f) {
            any_patch_available = true;
            break;
        }
    }

    for (size_t i = 0; i < state->count; ++i) {
        float x = state->x[i];
        float y = state->y[i];
        float vx = state->vx[i];
        float vy = state->vy[i];
        float heading = state->heading[i];
        float radius = state->radius[i];
        float energy = state->energy[i];
        float load = state->load_nectar[i];
        uint8_t prev_mode = state->mode[i];
        uint8_t prev_intent = state->intent[i];
        float prev_t_state = state->t_state[i];
        int32_t target_id = state->target_id[i];
        float target_x = state->target_pos_x[i];
        float target_y = state->target_pos_y[i];
        float capacity = state->capacity_uL[i] > 0.0f ? state->capacity_uL[i] : state->bee_capacity_uL;
        if (capacity <= 0.0f) {
            capacity = 50.0f;
        }
        float harvest_rate = state->harvest_rate_uLps[i] > 0.0f ? state->harvest_rate_uLps[i] : state->bee_harvest_rate_uLps;

        const FlowerPatch *target_patch = sim_get_patch_const(state, target_id);
        bool inside_hive_now = state->hive_enabled &&
                               x >= state->hive_rect_x &&
                               x <= state->hive_rect_x + state->hive_rect_w &&
                               y >= state->hive_rect_y &&
                               y <= state->hive_rect_y + state->hive_rect_h;

        float current_arrive_tol = arrive_tol;
        if (target_patch && (prev_mode == BEE_MODE_OUTBOUND || prev_mode == BEE_MODE_FORAGING ||
                             prev_intent == BEE_INTENT_FIND_PATCH || prev_intent == BEE_INTENT_HARVEST)) {
            float patch_tol = target_patch->radius * 0.6f;
            if (patch_tol > current_arrive_tol) {
                current_arrive_tol = patch_tol;
            }
        }

        float dx = target_x - x;
        float dy = target_y - y;
        bool arrived = (dx * dx + dy * dy) <= (current_arrive_tol * current_arrive_tol);

        BeeDecisionContext dctx = {
            .inside_hive = inside_hive_now,
            .arrived = arrived,
            .patch_valid = any_patch_available,
            .energy = energy,
            .load_uL = load,
            .capacity_uL = capacity,
            .patch_stock = target_patch ? target_patch->stock : 0.0f,
            .patch_capacity = target_patch ? target_patch->capacity : 0.0f,
            .patch_quality = target_patch ? target_patch->quality : 0.0f,
            .state_time = prev_t_state,
            .dt_sec = dt_sec,
            .hive_center_x = state->hive_rect_w > 0.0f ? state->hive_rect_x + state->hive_rect_w * 0.5f : world_w * 0.5f,
            .hive_center_y = state->hive_rect_h > 0.0f ? state->hive_rect_y + state->hive_rect_h * 0.5f : world_h * 0.5f,
            .entrance_x = entrance_x,
            .entrance_y = entrance_y,
            .unload_x = unload_x,
            .unload_y = unload_y,
            .forage_target_x = target_patch ? target_patch->x : target_x,
            .forage_target_y = target_patch ? target_patch->y : target_y,
            .arrive_tol = current_arrive_tol,
            .role = state->role[i],
            .previous_mode = prev_mode,
            .previous_intent = prev_intent,
            .patch_id = target_id,
        };
        BeeDecisionOutput decision = {0};
        bee_decide_next_action(&dctx, &decision);

        uint8_t intent = decision.intent;
        uint8_t mode = decision.mode;
        target_x = decision.target_x;
        target_y = decision.target_y;
        target_id = decision.target_id;
        bool mode_changed = (mode != prev_mode);

        if (mode == BEE_MODE_OUTBOUND || mode == BEE_MODE_FORAGING) {
            if (target_id < 0 || !sim_get_patch_const(state, target_id)) {
                target_id = choose_flower_patch(state, x, y, &rng);
                mode_changed = true;
            }
        }

        target_patch = sim_get_patch_const(state, target_id);
        if ((mode == BEE_MODE_OUTBOUND || mode == BEE_MODE_FORAGING) && !target_patch) {
            intent = BEE_INTENT_REST;
            mode = BEE_MODE_IDLE;
            target_id = -1;
        }

        if (mode == BEE_MODE_OUTBOUND && target_patch) {
            if (mode_changed || target_id != state->target_id[i]) {
                float sample_x = target_patch->x;
                float sample_y = target_patch->y;
                sample_patch_point(target_patch, &rng, &sample_x, &sample_y);
                target_x = sample_x;
                target_y = sample_y;
            }
        } else if (mode == BEE_MODE_FORAGING && target_patch) {
            target_x = target_patch->x;
            target_y = target_patch->y;
        } else if (mode == BEE_MODE_RETURNING || mode == BEE_MODE_ENTERING) {
            target_x = entrance_x;
            target_y = entrance_y;
        } else {
            target_x = unload_x;
            target_y = unload_y;
        }

        current_arrive_tol = arrive_tol;
        if (target_patch && mode == BEE_MODE_FORAGING) {
            float patch_tol = target_patch->radius * 0.5f;
            if (patch_tol > current_arrive_tol) {
                current_arrive_tol = patch_tol;
            }
        }

        dx = target_x - x;
        dy = target_y - y;
        float dist_sq = dx * dx + dy * dy;
        float distance = sqrtf(dist_sq);
        bool flight_mode = (mode == BEE_MODE_OUTBOUND || mode == BEE_MODE_RETURNING || mode == BEE_MODE_ENTERING);

        float desired_vx = 0.0f;
        float desired_vy = 0.0f;
        if (flight_mode) {
            if (distance > 1e-5f) {
                float inv_dist = 1.0f / distance;
                float dir_x = dx * inv_dist;
                float dir_y = dy * inv_dist;
                float jitter = 0.15f * rand_symmetric(&rng);
                float cos_j = cosf(jitter);
                float sin_j = sinf(jitter);
                float rot_x = dir_x * cos_j - dir_y * sin_j;
                float rot_y = dir_x * sin_j + dir_y * cos_j;
                desired_vx = rot_x * base_speed;
                desired_vy = rot_y * base_speed;
            }
        } else {
            vx *= 0.65f;
            vy *= 0.65f;
            if (fabsf(vx) < 1e-3f) vx = 0.0f;
            if (fabsf(vy) < 1e-3f) vy = 0.0f;
        }

        float dvx = desired_vx - vx;
        float dvy = desired_vy - vy;
        float delta_v = sqrtf(dvx * dvx + dvy * dvy);
        float max_delta = seek_accel * dt_sec;
        if (delta_v > max_delta && delta_v > 1e-6f) {
            float scale = max_delta / delta_v;
            dvx *= scale;
            dvy *= scale;
        }
        vx += dvx;
        vy += dvy;

        float speed = sqrtf(vx * vx + vy * vy);
        if (speed > max_speed && speed > 1e-6f) {
            float scale = max_speed / speed;
            vx *= scale;
            vy *= scale;
            speed = max_speed;
        }

        float new_x = x + vx * dt_sec;
        float new_y = y + vy * dt_sec;

        float min_x = radius + bounce_margin;
        float max_x = world_w - radius - bounce_margin;
        if (min_x > max_x) {
            float mid = world_w * 0.5f;
            min_x = max_x = mid;
        }
        if (new_x < min_x) {
            new_x = min_x;
            vx = -vx * 0.3f;
            ++bounce_counter;
        } else if (new_x > max_x) {
            new_x = max_x;
            vx = -vx * 0.3f;
            ++bounce_counter;
        }

        float min_y = radius + bounce_margin;
        float max_y = world_h - radius - bounce_margin;
        if (min_y > max_y) {
            float mid = world_h * 0.5f;
            min_y = max_y = mid;
        }
        if (new_y < min_y) {
            new_y = min_y;
            vy = -vy * 0.3f;
            ++bounce_counter;
        } else if (new_y > max_y) {
            new_y = max_y;
            vy = -vy * 0.3f;
            ++bounce_counter;
        }

        hive_resolve_disc(state, radius, &new_x, &new_y, &vx, &vy);

        float speed_after = sqrtf(vx * vx + vy * vy);
        bool inside_after = state->hive_enabled &&
                            new_x >= state->hive_rect_x &&
                            new_x <= state->hive_rect_x + state->hive_rect_w &&
                            new_y >= state->hive_rect_y &&
                            new_y <= state->hive_rect_y + state->hive_rect_h;

        if (inside_after && !inside_hive_now && (mode == BEE_MODE_RETURNING || mode == BEE_MODE_ENTERING)) {
            mode = BEE_MODE_ENTERING;
            target_x = unload_x;
            target_y = unload_y;
        }
        state->inside_hive_flag[i] = inside_after ? 1u : 0u;

        flight_mode = (mode == BEE_MODE_OUTBOUND || mode == BEE_MODE_RETURNING || mode == BEE_MODE_ENTERING);
        const float flight_cost = 0.0007f;
        const float forage_cost = 0.00025f;
        float rest_recovery = state->bee_rest_recovery_per_s > 0.0f ? state->bee_rest_recovery_per_s : 0.3f;
        if (flight_mode) {
            float load_factor = 1.0f + (capacity > 0.0f ? (load / capacity) * 0.25f : 0.0f);
            energy -= flight_cost * speed_after * load_factor * dt_sec;
        } else if (mode == BEE_MODE_FORAGING) {
            energy -= forage_cost * dt_sec;
        } else {
            energy += rest_recovery * dt_sec;
        }

        if (mode == BEE_MODE_FORAGING) {
            FlowerPatch *patch_mut = sim_get_patch(state, target_id);
            if (patch_mut && patch_mut->stock > 0.0f) {
                float patch_factor = 0.6f + 0.4f * patch_mut->quality;
                float harvest = harvest_rate * patch_factor * dt_sec;
                float space = capacity - load;
                if (harvest > space) harvest = space;
                if (harvest > patch_mut->stock) harvest = patch_mut->stock;
                if (harvest > 0.0f) {
                    load += harvest;
                    patch_mut->stock -= harvest;
                }
            }
        } else if (mode == BEE_MODE_UNLOADING) {
            float unload = state->bee_unload_rate_uLps * dt_sec;
            if (unload > load) unload = load;
            load -= unload;
        }

        if (energy < 0.0f) energy = 0.0f;
        if (energy > 1.0f) energy = 1.0f;
        if (load < 0.0f) load = 0.0f;
        if (load > capacity) load = capacity;

        if (mode != BEE_MODE_OUTBOUND && mode != BEE_MODE_FORAGING) {
            target_id = -1;
        }

        state->x[i] = new_x;
        state->y[i] = new_y;
        state->vx[i] = vx;
        state->vy[i] = vy;
        if (speed_after > 1e-5f) {
            heading = wrap_angle(atan2f(vy, vx));
        }
        state->heading[i] = heading;

        if (speed_after < speed_min_tick) {
            speed_min_tick = speed_after;
        }
        if (speed_after > speed_max_tick) {
            speed_max_tick = speed_after;
        }
        speed_sum += speed_after;

        state->energy[i] = energy;
        state->load_nectar[i] = load;
        state->intent[i] = intent;
        state->mode[i] = mode;
        state->color_rgba[i] = bee_mode_color(mode);
        state->target_pos_x[i] = target_x;
        state->target_pos_y[i] = target_y;
        state->target_id[i] = target_id;
        state->t_state[i] = (mode == prev_mode) ? prev_t_state + dt_sec : 0.0f;
        state->age_days[i] += dt_sec / 86400.0f;
        float conf = (float)state->topic_confidence[i];
        conf -= dt_sec * 20.0f;
        if (conf < 0.0f) conf = 0.0f;
        if (conf > 255.0f) conf = 255.0f;
        state->topic_confidence[i] = (uint8_t)(conf + 0.5f);
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
        LOG_INFO("sim: n=%zu dt=%.5f speed=%.1f jitter=%.1fdeg/s avg=%.1f min=%.1f max=%.1f bounces=%llu",
                 state->count,
                 dt_sec,
                 base_speed,
                 jitter_deg,
                 (float)avg_speed,
                 (float)min_speed_log,
                 (float)max_speed_log,
                 (unsigned long long)state->log_bounce_count);
        reset_log_stats(state);
    }
}

RenderView sim_build_view(SimState *state) {
    RenderView view = (RenderView){0};
    if (!state) {
        return view;
    }
    view.count = state->count;
    view.positions_xy = state->scratch_xy;
    view.radii_px = state->radius;
    view.color_rgba = state->color_rgba;
    view.patch_positions_xy = state->patch_positions_xy;
    view.patch_radii_px = state->patch_radii_px;
    view.patch_fill_rgba = state->patch_fill_rgba;
    view.patch_ring_radii_px = state->patch_ring_radii_px;
    view.patch_ring_rgba = state->patch_ring_rgba;
    view.patch_count = state->patch_count;

    for (size_t i = 0; i < state->patch_count && i < SIM_MAX_FLOWER_PATCHES; ++i) {
        const FlowerPatch *patch = &state->patches[i];
        state->patch_positions_xy[2 * i + 0] = patch->x;
        state->patch_positions_xy[2 * i + 1] = patch->y;
        state->patch_radii_px[i] = patch->radius;

        float quality = clampf(patch->quality, 0.0f, 1.0f);
        float fill_r = 0.18f + 0.10f * (1.0f - quality);
        float fill_g = 0.45f + 0.30f * quality;
        float fill_b = 0.18f;
        float fill_a = 0.16f;
        state->patch_fill_rgba[i] = make_color(fill_r, fill_g, fill_b, fill_a);

        float stock_ratio = (patch->capacity > 0.0f) ? patch->stock / patch->capacity : 0.0f;
        if (stock_ratio < 0.0f) stock_ratio = 0.0f;
        if (stock_ratio > 1.0f) stock_ratio = 1.0f;

        float ring_r = 0.75f - 0.45f * stock_ratio;
        float ring_g = 0.25f + 0.50f * stock_ratio;
        float ring_b = 0.10f;
        float ring_a = 0.85f;
        state->patch_ring_rgba[i] = make_color(ring_r, ring_g, ring_b, ring_a);
        state->patch_ring_radii_px[i] = patch->radius * stock_ratio;
    }

    return view;
}

void sim_apply_runtime_params(SimState *state, const Params *params) {
    if (!state || !params) {
        return;
    }

    float min_speed = params->motion_min_speed;
    if (min_speed <= 0.0f) {
        min_speed = state->min_speed > 0.0f ? state->min_speed : 1.0f;
    }
    float max_speed = params->motion_max_speed;
    if (max_speed < min_speed) {
        max_speed = min_speed;
    }

    state->min_speed = min_speed;
    state->max_speed = max_speed;
    float jitter_rad = params->motion_jitter_deg_per_sec * (float)M_PI / 180.0f;
    if (jitter_rad < 0.0f) {
        jitter_rad = 0.0f;
    }
    state->jitter_rad_per_sec = jitter_rad;

    float bounce_margin = params->motion_bounce_margin;
    if (bounce_margin < 0.0f) {
        bounce_margin = 0.0f;
    }
    state->bounce_margin = bounce_margin;
    state->spawn_speed_mean = params->motion_spawn_speed_mean;
    if (state->spawn_speed_mean < 0.0f) {
        state->spawn_speed_mean = 0.0f;
    }
    state->spawn_speed_std = params->motion_spawn_speed_std;
    if (state->spawn_speed_std < 0.0f) {
        state->spawn_speed_std = 0.0f;
    }
    state->spawn_mode = params->motion_spawn_mode;

    state->hive_rect_x = params->hive.rect_x;
    state->hive_rect_y = params->hive.rect_y;
    state->hive_rect_w = params->hive.rect_w;
    state->hive_rect_h = params->hive.rect_h;
    state->hive_entrance_side = params->hive.entrance_side;
    state->hive_entrance_t = params->hive.entrance_t;
    state->hive_entrance_width = params->hive.entrance_width;
    state->hive_restitution = params->hive.restitution;
    state->hive_tangent_damp = params->hive.tangent_damp;
    state->hive_max_iters = params->hive.max_resolve_iters;
    state->hive_safety_margin = params->hive.safety_margin;
    state->bee_capacity_uL = params->bee.capacity_uL;
    state->bee_harvest_rate_uLps = params->bee.harvest_rate_uLps;
    state->bee_unload_rate_uLps = params->bee.unload_rate_uLps;
    state->bee_rest_recovery_per_s = params->bee.rest_recovery_per_s;
    state->bee_speed_mps = params->bee.speed_mps;
    state->bee_seek_accel = params->bee.seek_accel;
    state->bee_arrive_tol_world = params->bee.arrive_tol_world;
    sim_build_hive_segments(state);

    for (size_t i = 0; i < state->count; ++i) {
        state->capacity_uL[i] = state->bee_capacity_uL;
        state->harvest_rate_uLps[i] = state->bee_harvest_rate_uLps;
    }

    const float world_w = state->world_w;
    const float world_h = state->world_h;

    for (size_t i = 0; i < state->count; ++i) {
        float vx = state->vx[i];
        float vy = state->vy[i];
        float speed_sq = vx * vx + vy * vy;
        float heading = state->heading[i];
        if (speed_sq > 0.0f) {
            float speed = sqrtf(speed_sq);
            if (speed > max_speed && max_speed > 0.0f) {
                float scale = max_speed / speed;
                vx *= scale;
                vy *= scale;
                speed = max_speed;
            } else if (speed < min_speed) {
                float scale = min_speed / speed;
                vx *= scale;
                vy *= scale;
                speed = min_speed;
            }
            heading = atan2f(vy, vx);
        } else {
            if (!isfinite(heading)) {
                heading = 0.0f;
            }
            vx = cosf(heading) * min_speed;
            vy = sinf(heading) * min_speed;
        }

        state->vx[i] = vx;
        state->vy[i] = vy;
        state->heading[i] = heading;

        float radius = state->radius ? state->radius[i] : state->default_radius;
        float min_x = radius + state->bounce_margin;
        float max_x = world_w - radius - state->bounce_margin;
        if (min_x > max_x) {
            float mid = world_w * 0.5f;
            min_x = mid;
            max_x = mid;
        }
        float min_y = radius + state->bounce_margin;
        float max_y = world_h - radius - state->bounce_margin;
        if (min_y > max_y) {
            float mid = world_h * 0.5f;
            min_y = mid;
            max_y = mid;
        }

        float x = state->x[i];
        float y = state->y[i];
        if (x < min_x) {
            x = min_x;
        } else if (x > max_x) {
            x = max_x;
        }
        if (y < min_y) {
            y = min_y;
        } else if (y > max_y) {
            y = max_y;
        }

        state->x[i] = x;
        state->y[i] = y;
    }

    update_scratch(state);
    reset_log_stats(state);
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

size_t sim_find_bee_near(const SimState *state, float world_x, float world_y, float radius_world) {
    if (!state || state->count == 0 || radius_world <= 0.0f) {
        return SIZE_MAX;
    }
    float best_dist_sq = radius_world * radius_world;
    size_t best_index = SIZE_MAX;
    for (size_t i = 0; i < state->count; ++i) {
        float dx = state->x[i] - world_x;
        float dy = state->y[i] - world_y;
        float combined = radius_world + state->radius[i];
        float limit_sq = combined * combined;
        float dist_sq = dx * dx + dy * dy;
        if (dist_sq <= limit_sq && dist_sq <= best_dist_sq) {
            best_dist_sq = dist_sq;
            best_index = i;
        }
    }
    return best_index;
}

bool sim_get_bee_info(const SimState *state, size_t index, BeeDebugInfo *out_info) {
    if (!state || !out_info || index >= state->count) {
        return false;
    }
    BeeDebugInfo info = {0};
    info.index = index;
    info.pos_x = state->x[index];
    info.pos_y = state->y[index];
    info.vel_x = state->vx[index];
    info.vel_y = state->vy[index];
    info.speed = sqrtf(state->vx[index] * state->vx[index] + state->vy[index] * state->vy[index]);
    info.radius = state->radius[index];
    info.age_days = state->age_days[index];
    info.state_time = state->t_state[index];
    info.energy = state->energy[index];
    info.load_nectar = state->load_nectar[index];
    info.capacity_uL = state->capacity_uL[index];
    info.harvest_rate_uLps = state->harvest_rate_uLps[index];
    info.target_pos_x = state->target_pos_x[index];
    info.target_pos_y = state->target_pos_y[index];
    info.target_id = state->target_id[index];
    info.topic_id = state->topic_id[index];
    info.topic_confidence = state->topic_confidence[index];
    info.role = state->role[index];
    info.mode = state->mode[index];
    info.intent = state->intent[index];

    bool inside_hive = false;
    if (state->hive_enabled && state->hive_rect_w > 0.0f && state->hive_rect_h > 0.0f) {
        inside_hive = (info.pos_x >= state->hive_rect_x &&
                       info.pos_x <= state->hive_rect_x + state->hive_rect_w &&
                       info.pos_y >= state->hive_rect_y &&
                       info.pos_y <= state->hive_rect_y + state->hive_rect_h);
    }
    info.inside_hive = inside_hive;

    *out_info = info;
    return true;
}

