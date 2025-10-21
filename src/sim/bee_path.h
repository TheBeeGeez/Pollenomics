#ifndef SIM_BEE_PATH_H
#define SIM_BEE_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct SimState;

typedef struct BeePathPlan {
    float dir_x;
    float dir_y;
    float waypoint_x;
    float waypoint_y;
    float final_x;
    float final_y;
    uint8_t has_waypoint;
    uint8_t valid;
} BeePathPlan;

bool bee_path_plan(const struct SimState *state,
                   size_t index,
                   float target_x,
                   float target_y,
                   float arrive_tol,
                   BeePathPlan *out_plan);

#endif  // SIM_BEE_PATH_H
