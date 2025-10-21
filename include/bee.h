#ifndef BEE_H
#define BEE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum BeeRole {
    BEE_ROLE_NURSE = 0,
    BEE_ROLE_HOUSEKEEPER = 1,
    BEE_ROLE_STORAGE = 2,
    BEE_ROLE_FORAGER = 3,
    BEE_ROLE_SCOUT = 4,
    BEE_ROLE_GUARD = 5,
    BEE_ROLE_QUEEN = 6,
} BeeRole;

typedef enum BeeMode {
    BEE_MODE_IDLE = 0,
    BEE_MODE_OUTBOUND = 1,
    BEE_MODE_FORAGING = 2,
    BEE_MODE_RETURNING = 3,
    BEE_MODE_ENTERING = 4,
    BEE_MODE_UNLOADING = 5,
} BeeMode;

typedef enum BeeIntent {
    BEE_INTENT_FIND_PATCH = 0,
    BEE_INTENT_HARVEST = 1,
    BEE_INTENT_RETURN_HOME = 2,
    BEE_INTENT_UNLOAD = 3,
    BEE_INTENT_REST = 4,
    BEE_INTENT_EXPLORE = 5,
} BeeIntent;

typedef struct BeeDebugInfo {
    size_t index;
    float pos_x;
    float pos_y;
    float vel_x;
    float vel_y;
    float speed;
    float radius;
    float age_days;
    float state_time;
    float energy;
    float load_nectar;
    float capacity_uL;
    float harvest_rate_uLps;
    float target_pos_x;
    float target_pos_y;
    int32_t target_id;
    int16_t topic_id;
    uint8_t topic_confidence;
    uint8_t role;
    uint8_t mode;
    uint8_t intent;
    bool inside_hive;
} BeeDebugInfo;

typedef struct BeeDecisionContext {
    bool inside_hive;
    bool arrived;
    bool patch_valid;
    float energy;
    float load_uL;
    float capacity_uL;
    float patch_stock;
    float patch_capacity;
    float patch_quality;
    float state_time;
    float dt_sec;
    float hive_center_x;
    float hive_center_y;
    float entrance_x;
    float entrance_y;
    float unload_x;
    float unload_y;
    float forage_target_x;
    float forage_target_y;
    float arrive_tol;
    uint8_t role;
    uint8_t previous_mode;
    uint8_t previous_intent;
    int32_t patch_id;
} BeeDecisionContext;

typedef struct BeeDecisionOutput {
    uint8_t intent;
    uint8_t mode;
    float target_x;
    float target_y;
    int32_t target_id;
} BeeDecisionOutput;

BeeRole bee_pick_role(float age_days, uint64_t *rng_state);

void bee_decide_next_action(const BeeDecisionContext *ctx, BeeDecisionOutput *out);

#endif  // BEE_H
