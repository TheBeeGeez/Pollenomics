#ifndef SIM_SIM_INTERNAL_H
#define SIM_SIM_INTERNAL_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

static inline float clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static inline uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static inline float rand_uniform01(uint64_t *state) {
    uint64_t x = xorshift64(state);
    return (float)((x >> 11) * (1.0 / 9007199254740992.0));
}

static inline float rand_symmetric(uint64_t *state) {
    return rand_uniform01(state) * 2.0f - 1.0f;
}

#endif  // SIM_SIM_INTERNAL_H
