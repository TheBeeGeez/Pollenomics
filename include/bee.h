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
} BeeRole;

typedef enum BeeMode {
    BEE_MODE_IDLE = 0,
    BEE_MODE_FORAGING = 1,
    BEE_MODE_RETURNING = 2,
    BEE_MODE_APPROACH_ENTRANCE = 3,
    BEE_MODE_INSIDE_MOVE = 4,
    BEE_MODE_UNLOAD_WAIT = 5,
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
    float energy;
    float load_nectar;
    uint8_t role;
    uint8_t previous_mode;
    uint8_t previous_intent;
    float hive_center_x;
    float hive_center_y;
    float world_width;
    float world_height;
    float forage_target_x;
    float forage_target_y;
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
