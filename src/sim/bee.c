git #include "bee.h"

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

    uint8_t intent = ctx->previous_intent;
    uint8_t mode = ctx->previous_mode;
    const bool inside_hive = ctx->inside_hive;
    const float energy = ctx->energy;
    const float load = ctx->load_nectar;

    if (energy < 0.2f) {
        intent = BEE_INTENT_REST;
    } else if (load >= 0.8f) {
        intent = BEE_INTENT_RETURN_HOME;
    } else if (ctx->previous_intent == BEE_INTENT_REST && energy > 0.6f) {
        intent = (ctx->role == BEE_ROLE_FORAGER || ctx->role == BEE_ROLE_SCOUT)
                     ? BEE_INTENT_FIND_PATCH
                     : BEE_INTENT_UNLOAD;
    } else if (intent != BEE_INTENT_FIND_PATCH && intent != BEE_INTENT_EXPLORE &&
               intent != BEE_INTENT_HARVEST && !inside_hive) {
        intent = BEE_INTENT_FIND_PATCH;
    }

    if (inside_hive && load <= 0.05f &&
        (intent == BEE_INTENT_RETURN_HOME || intent == BEE_INTENT_UNLOAD)) {
        intent = (energy > 0.4f) ? BEE_INTENT_FIND_PATCH : BEE_INTENT_REST;
    }

    if (inside_hive) {
        mode = (intent == BEE_INTENT_RETURN_HOME || intent == BEE_INTENT_UNLOAD)
                   ? BEE_MODE_UNLOAD_WAIT
                   : BEE_MODE_INSIDE_MOVE;
    } else if (intent == BEE_INTENT_RETURN_HOME || intent == BEE_INTENT_REST) {
        mode = BEE_MODE_RETURNING;
    } else {
        mode = BEE_MODE_FORAGING;
    }

    float target_x = ctx->forage_target_x;
    float target_y = ctx->forage_target_y;
    int32_t target_id = -1;

    if (intent == BEE_INTENT_RETURN_HOME || intent == BEE_INTENT_UNLOAD || intent == BEE_INTENT_REST) {
        target_x = ctx->hive_center_x;
        target_y = ctx->hive_center_y;
    } else if (intent == BEE_INTENT_FIND_PATCH || intent == BEE_INTENT_HARVEST || intent == BEE_INTENT_EXPLORE) {
        target_x = ctx->forage_target_x;
        target_y = ctx->forage_target_y;
    }

    result.intent = intent;
    result.mode = mode;
    result.target_x = target_x;
    result.target_y = target_y;
    result.target_id = target_id;
    *out = result;
}
