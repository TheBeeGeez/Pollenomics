#ifndef PATH_PATH_FIELDS_H
#define PATH_PATH_FIELDS_H

#include <stddef.h>
#include <stdint.h>

#include "path/path.h"

const float *path_field_dist(PathGoal goal);
const uint8_t *path_field_next(PathGoal goal);
uint32_t path_field_stamp(PathGoal goal);
size_t path_field_tile_count(void);

#endif  // PATH_PATH_FIELDS_H
