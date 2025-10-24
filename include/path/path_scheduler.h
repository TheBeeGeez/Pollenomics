#ifndef PATH_PATH_SCHEDULER_H
#define PATH_PATH_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "path/path.h"

void path_sched_set_budget_ms(float per_frame_ms);
void path_sched_set_cadence(PathGoal goal, float Hz);
float path_sched_get_last_build_ms(PathGoal goal);
size_t path_sched_get_last_relaxed(PathGoal goal);
bool path_sched_is_building(PathGoal goal);
uint32_t path_sched_get_stamp(PathGoal goal);
void path_sched_force_full_recompute(PathGoal goal);
size_t path_sched_get_dirty_queue_len(void);
size_t path_sched_get_dirty_processed_last_build(PathGoal goal);

#endif  // PATH_PATH_SCHEDULER_H
