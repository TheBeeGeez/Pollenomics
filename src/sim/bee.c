#include "bee.h"

static float bee_rand_uniform01(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return (float)((x >> 11) * (1.0 / 9007199254740992.0));
}

BeeRole bee_pick_role(float age_days, uint64_t *rng_state) {
    if (age_days < 6.0f) {
        return BEE_ROLE_NURSE;
    }
    if (age_days < 12.0f) {
        return BEE_ROLE_HOUSEKEEPER;
    }
    if (age_days < 18.0f) {
        return BEE_ROLE_STORAGE;
    }
    if (!rng_state) {
        return BEE_ROLE_FORAGER;
    }
    float roll = bee_rand_uniform01(rng_state);
    if (roll < 0.12f) {
        return BEE_ROLE_SCOUT;
    }
    if (roll < 0.18f) {
        return BEE_ROLE_GUARD;
    }
    return BEE_ROLE_FORAGER;
}

void bee_decide_next_action(const BeeDecisionContext *ctx, BeeDecisionOutput *out) {
    if (!out) {
        return;
    }

    BeeDecisionOutput result = {
        .intent = BEE_INTENT_REST,
        .mode = BEE_MODE_IDLE,
        .target_x = 0.0f,
        .target_y = 0.0f,
        .target_id = -1,
    };

    if (!ctx) {
        *out = result;
        return;
    }

    const float capacity = ctx->capacity_uL > 0.0f ? ctx->capacity_uL : 1.0f;
    const float load_ratio = ctx->load_uL / capacity;
    const float load_empty_threshold = 0.05f * capacity;
    const float load_full_threshold = 0.95f;
    const float energy_low = 0.28f;
    const float energy_high = 0.82f;
    const float min_rest_time = 2.0f;
    const float min_forage_time = 2.0f;
    const float max_forage_time = 30.0f;

    uint8_t intent = ctx->previous_intent;
    uint8_t mode = ctx->previous_mode;
    int32_t target_id = ctx->patch_id;
    if (target_id < 0) {
        target_id = -1;
    }
    float target_x = ctx->forage_target_x;
    float target_y = ctx->forage_target_y;
    const bool forage_capable = (ctx->role == BEE_ROLE_FORAGER || ctx->role == BEE_ROLE_SCOUT);

    if (!ctx->inside_hive) {
        if (ctx->energy <= energy_low || load_ratio >= load_full_threshold) {
            intent = BEE_INTENT_RETURN_HOME;
        } else if (ctx->arrived && ctx->patch_id >= 0) {
            intent = BEE_INTENT_HARVEST;
        } else if (intent != BEE_INTENT_FIND_PATCH && intent != BEE_INTENT_HARVEST) {
            intent = BEE_INTENT_FIND_PATCH;
        }
    } else {
        if (ctx->load_uL > load_empty_threshold) {
            intent = BEE_INTENT_UNLOAD;
        } else {
            if (intent == BEE_INTENT_UNLOAD) {
                intent = BEE_INTENT_REST;
            }
            if (intent == BEE_INTENT_REST) {
                if (ctx->state_time < min_rest_time || ctx->energy < energy_high || !forage_capable) {
                    intent = BEE_INTENT_REST;
                } else if (ctx->patch_valid) {
                    intent = BEE_INTENT_FIND_PATCH;
                }
            } else if (forage_capable && ctx->energy >= energy_high && ctx->patch_valid) {
                intent = BEE_INTENT_FIND_PATCH;
            } else {
                intent = BEE_INTENT_REST;
            }
        }
    }

    if (intent == BEE_INTENT_HARVEST) {
        if (ctx->patch_id < 0 || ctx->patch_stock <= 1e-3f) {
            intent = ctx->inside_hive ? BEE_INTENT_UNLOAD : BEE_INTENT_RETURN_HOME;
        } else {
            if (ctx->state_time < min_forage_time) {
                intent = BEE_INTENT_HARVEST;
            } else if (load_ratio >= load_full_threshold ||
                       ctx->patch_stock <= 0.1f * ctx->patch_capacity ||
                       ctx->energy <= energy_low ||
                       ctx->state_time >= max_forage_time) {
                intent = BEE_INTENT_RETURN_HOME;
            }
        }
    }

    if (intent == BEE_INTENT_RETURN_HOME && ctx->inside_hive && ctx->arrived) {
        intent = (ctx->load_uL > load_empty_threshold) ? BEE_INTENT_UNLOAD : BEE_INTENT_REST;
    }

    switch (intent) {
        case BEE_INTENT_FIND_PATCH:
        case BEE_INTENT_HARVEST:
        case BEE_INTENT_EXPLORE:
            target_x = ctx->forage_target_x;
            target_y = ctx->forage_target_y;
            target_id = ctx->patch_id;
            if (target_id < 0) {
                target_id = -1;
            }
            mode = (intent == BEE_INTENT_HARVEST) ? BEE_MODE_FORAGING : BEE_MODE_OUTBOUND;
            break;
        case BEE_INTENT_RETURN_HOME:
            target_x = ctx->entrance_x;
            target_y = ctx->entrance_y;
            target_id = -1;
            mode = ctx->inside_hive ? BEE_MODE_ENTERING : BEE_MODE_RETURNING;
            break;
        case BEE_INTENT_UNLOAD:
            target_x = ctx->unload_x;
            target_y = ctx->unload_y;
            target_id = -1;
            mode = BEE_MODE_UNLOADING;
            break;
        case BEE_INTENT_REST:
        default:
            target_x = ctx->unload_x;
            target_y = ctx->unload_y;
            target_id = -1;
            mode = BEE_MODE_IDLE;
            break;
    }

    result.intent = intent;
    result.mode = mode;
    result.target_x = target_x;
    result.target_y = target_y;
    result.target_id = target_id;
    *out = result;
}



