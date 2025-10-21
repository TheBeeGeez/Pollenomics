#include "sim.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#include <malloc.h>
#endif

#include "util/log.h"

#include "sim_internal.h"
#include "hive.h"
#include "plants.h"

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

static uint32_t bee_color_for(uint8_t role, uint8_t mode) {
    if (role == (uint8_t)BEE_ROLE_QUEEN) {
        return make_color(0.95f, 0.30f, 0.85f, 1.0f);
    }
    return bee_mode_color(mode);
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
    hive_build_segments(state);

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
    hive_compute_points(state, &entrance_x, &entrance_y, &unload_x, &unload_y);

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

    plants_generate(state, &rng);

    for (size_t i = 0; i < state->count; ++i) {
        size_t col = i % cols;
        size_t row = i / cols;

        float base_x = origin_x + (float)col * spacing;
        float base_y = origin_y + (float)row * spacing;

        float jitter_x = rand_symmetric(&rng) * bee_radius * 0.25f;
        float jitter_y = rand_symmetric(&rng) * bee_radius * 0.25f;

        float x = base_x + jitter_x;
        float y = base_y + jitter_y;

        if (i == 0 && state->hive_enabled) {
            x = unload_x;
            y = unload_y;
        }

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

        BeeRole role = (i == 0) ? BEE_ROLE_QUEEN : bee_pick_role(age_days, &rng);
        state->role[i] = (uint8_t)role;

        state->mode[i] = (uint8_t)BEE_MODE_IDLE;
        state->intent[i] = (uint8_t)BEE_INTENT_REST;
        state->color_rgba[i] = bee_color_for(state->role[i], state->mode[i]);

        bool inside = state->hive_enabled &&
                      x >= state->hive_rect_x &&
                      x <= state->hive_rect_x + state->hive_rect_w &&
                      y >= state->hive_rect_y &&
                      y <= state->hive_rect_y + state->hive_rect_h;
        state->inside_hive_flag[i] = inside ? 1u : 0u;
        if (i == 0 && state->hive_enabled) {
            state->inside_hive_flag[i] = 1u;
        }
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

    plants_replenish(state, dt_sec);

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
    hive_compute_points(state, &entrance_x, &entrance_y, &unload_x, &unload_y);

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

        const FlowerPatch *target_patch = plants_get_patch_const(state, target_id);
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
            if (target_id < 0 || !plants_get_patch_const(state, target_id)) {
                target_id = plants_choose_patch(state, x, y, &rng);
                mode_changed = true;
            }
        }

        target_patch = plants_get_patch_const(state, target_id);
        if ((mode == BEE_MODE_OUTBOUND || mode == BEE_MODE_FORAGING) && !target_patch) {
            intent = BEE_INTENT_REST;
            mode = BEE_MODE_IDLE;
            target_id = -1;
        }

        if (mode == BEE_MODE_OUTBOUND && target_patch) {
            if (mode_changed || target_id != state->target_id[i]) {
                float sample_x = target_patch->x;
                float sample_y = target_patch->y;
                plants_sample_point(target_patch, &rng, &sample_x, &sample_y);
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
            FlowerPatch *patch_mut = plants_get_patch(state, target_id);
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
        state->color_rgba[i] = bee_color_for(state->role[i], mode);
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
    hive_build_segments(state);

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

