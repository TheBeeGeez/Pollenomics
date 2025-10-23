#ifndef PATH_PATH_INTERNAL_H
#define PATH_PATH_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "path/path.h"
#include "path/path_debug.h"
#include "path/path_fields.h"

struct HexWorld;

bool path_fields_init_storage(size_t tile_count);
void path_fields_shutdown_storage(void);
bool path_fields_build(PathGoal goal,
                       const struct HexWorld *world,
                       const int32_t *neighbors,
                       const TileId *goals,
                       size_t goal_count);

const float *path_core_direction_world(uint8_t dir_index);

bool path_debug_init(void);
void path_debug_shutdown(void);
void path_debug_reset_overlay(void);
bool path_debug_build_overlay(const struct HexWorld *world,
                              PathGoal goal,
                              const uint8_t *next,
                              size_t tile_count);

#endif  // PATH_PATH_INTERNAL_H
