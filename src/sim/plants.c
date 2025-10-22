#include "plants.h"

#include <float.h>

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

void plants_generate(SimState *state, uint64_t *rng_state) {
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

FlowerPatch *plants_get_patch(SimState *state, int32_t patch_id) {
    if (!state || patch_id < 0) {
        return NULL;
    }
    if ((size_t)patch_id >= state->patch_count) {
        return NULL;
    }
    return &state->patches[patch_id];
}

const FlowerPatch *plants_get_patch_const(const SimState *state, int32_t patch_id) {
    if (!state || patch_id < 0) {
        return NULL;
    }
    if ((size_t)patch_id >= state->patch_count) {
        return NULL;
    }
    return &state->patches[patch_id];
}

void plants_replenish(SimState *state, float dt_sec) {
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

void plants_sample_point(const FlowerPatch *patch, uint64_t *rng, float *out_x, float *out_y) {
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

int32_t plants_choose_patch(const SimState *state,
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
