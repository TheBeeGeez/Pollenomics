#ifndef PATH_PATH_INTERNAL_H
#define PATH_PATH_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "path/path.h"
#include "path/path_cost.h"
#include "path/path_debug.h"
#include "path/path_fields.h"

struct HexWorld;

bool path_fields_init_storage(size_t tile_count);
void path_fields_shutdown_storage(void);
bool path_fields_start_build(PathGoal goal,
                             const struct HexWorld *world,
                             const int32_t *neighbors,
                             const TileId *goals,
                             size_t goal_count,
                             const float *eff_cost,
                             const TileId *dirty_tiles,
                             size_t dirty_count);
bool path_fields_step(PathGoal goal,
                      double time_budget_ms,
                      size_t *out_relaxed,
                      double *out_elapsed_ms,
                      bool *out_finished);
bool path_fields_is_building(PathGoal goal);
void path_fields_cancel_build(PathGoal goal);

const float *path_core_direction_world(uint8_t dir_index);

bool path_debug_init(void);
void path_debug_shutdown(void);
void path_debug_reset_overlay(void);
bool path_debug_build_overlay(const struct HexWorld *world,
                              PathGoal goal,
                              const uint8_t *next,
                              size_t tile_count);

void path_sched_reset_state(void);
void path_sched_shutdown_state(void);
void path_sched_set_goal_data(PathGoal goal,
                              const struct HexWorld *world,
                              const int32_t *neighbors,
                              const TileId *goals,
                              size_t goal_count,
                              size_t tile_count);
bool path_sched_update(float dt_sec, bool *out_field_swapped);

bool path_cost_init(const struct HexWorld *world);
void path_cost_shutdown(void);
const float *path_cost_eff_costs(void);
size_t path_cost_tile_count(void);
size_t path_cost_dirty_count(void);
size_t path_cost_consume_dirty(TileId *out, size_t max_tiles);
void path_cost_requeue_tiles(const TileId *tiles, size_t count);

#endif  // PATH_PATH_INTERNAL_H
