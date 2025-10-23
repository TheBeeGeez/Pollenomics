#ifndef PATH_PATH_H
#define PATH_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hex.h"
#include "params.h"
#include "tile_core.h"

typedef enum PathGoal {
    PATH_GOAL_ENTRANCE = 0,
    PATH_GOAL_UNLOAD = 1,
    PATH_GOAL_FLOWERS_NEAR = 2,
    PATH_GOAL_COUNT
} PathGoal;

typedef struct PathVec2 {
    float x;
    float y;
} PathVec2;

bool path_init(const HexWorld *world, const Params *params);
void path_shutdown(void);

void path_update(const HexWorld *world, const Params *params, float dt_sec);

void path_force_recompute(PathGoal goal);

bool path_query_direction(PathGoal goal, TileId nid, PathVec2 *dir_world_out);

#endif  // PATH_PATH_H
