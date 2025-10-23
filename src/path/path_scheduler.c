#include "path/path_scheduler.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "path/path_fields.h"
#include "path/path_internal.h"
#include "util/log.h"

typedef struct PathSchedGoalState {
    float cadence_hz;
    double cadence_interval_ms;
    double time_since_last_start_ms;
    bool building;
    bool pending_force;
    bool has_data;
    const HexWorld *world;
    const int32_t *neighbors;
    const TileId *goals;
    size_t goal_count;
    size_t tile_count;
    const float *goal_seed_costs;
    size_t nodes_relaxed_accum;
    double elapsed_ms_accum;
    float last_build_ms;
    size_t last_relaxed;
    size_t last_dirty_processed;
    size_t dirty_seed_count_active;
} PathSchedGoalState;

typedef struct PathSchedulerState {
    float budget_ms;
    PathSchedGoalState goals[PATH_GOAL_COUNT];
    TileId *shared_dirty_tiles;
    size_t shared_dirty_capacity;
    size_t shared_dirty_count;
    bool shared_dirty_valid;
    bool shared_dirty_used[PATH_GOAL_COUNT];
} PathSchedulerState;

static PathSchedulerState g_sched = {0};

static const float kDefaultCadenceHz[] = {10.0f, 6.0f, 3.0f};

static inline double cadence_to_interval(float hz) {
    return (hz > 0.0f) ? (1000.0 / (double)hz) : 0.0;
}

static PathSchedGoalState *get_goal_state(PathGoal goal) {
    if (goal < 0 || goal >= PATH_GOAL_COUNT) {
        return NULL;
    }
    return &g_sched.goals[goal];
}

static void clear_goal_state(PathSchedGoalState *state, int goal_index) {
    if (!state) {
        return;
    }
    if (path_fields_is_building((PathGoal)goal_index)) {
        path_fields_cancel_build((PathGoal)goal_index);
    }
    state->cadence_hz = (goal_index < (int)(sizeof kDefaultCadenceHz / sizeof kDefaultCadenceHz[0]))
                            ? kDefaultCadenceHz[goal_index]
                            : 0.0f;
    state->cadence_interval_ms = cadence_to_interval(state->cadence_hz);
    state->time_since_last_start_ms = 0.0;
    state->building = false;
    state->pending_force = false;
    state->has_data = false;
    state->world = NULL;
    state->neighbors = NULL;
    state->goals = NULL;
    state->goal_count = 0u;
    state->tile_count = 0u;
    state->goal_seed_costs = NULL;
    state->nodes_relaxed_accum = 0u;
    state->elapsed_ms_accum = 0.0;
    state->last_build_ms = 0.0f;
    state->last_relaxed = 0u;
    state->last_dirty_processed = 0u;
    state->dirty_seed_count_active = 0u;
}

static void reset_shared_batch(void) {
    g_sched.shared_dirty_count = 0u;
    g_sched.shared_dirty_valid = false;
    memset(g_sched.shared_dirty_used, 0, sizeof(g_sched.shared_dirty_used));
}

static bool ensure_shared_capacity(size_t needed) {
    if (needed == 0u) {
        return true;
    }
    if (g_sched.shared_dirty_capacity >= needed) {
        return true;
    }
    size_t new_capacity = g_sched.shared_dirty_capacity ? g_sched.shared_dirty_capacity : 256u;
    while (new_capacity < needed) {
        new_capacity *= 2u;
    }
    TileId *tiles = (TileId *)realloc(g_sched.shared_dirty_tiles, new_capacity * sizeof(TileId));
    if (!tiles) {
        LOG_ERROR("path_sched: failed to grow shared dirty buffer (capacity=%zu)", new_capacity);
        return false;
    }
    g_sched.shared_dirty_tiles = tiles;
    g_sched.shared_dirty_capacity = new_capacity;
    return true;
}

static bool ensure_shared_dirty_batch(size_t max_needed) {
    if (g_sched.shared_dirty_valid) {
        return g_sched.shared_dirty_count > 0u;
    }
    size_t available = path_cost_dirty_count();
    if (available == 0u) {
        return false;
    }
    size_t request = available;
    if (max_needed > 0u && request > max_needed) {
        request = max_needed;
    }
    if (!ensure_shared_capacity(request)) {
        return false;
    }
    size_t consumed = path_cost_consume_dirty(g_sched.shared_dirty_tiles, request);
    if (consumed == 0u) {
        return false;
    }
    g_sched.shared_dirty_count = consumed;
    g_sched.shared_dirty_valid = true;
    memset(g_sched.shared_dirty_used, 0, sizeof(g_sched.shared_dirty_used));
    return true;
}

static void finalize_shared_batch_if_consumed(void) {
    if (!g_sched.shared_dirty_valid) {
        return;
    }
    for (int goal = 0; goal < PATH_GOAL_COUNT; ++goal) {
        const PathSchedGoalState *state = &g_sched.goals[goal];
        if (!state->has_data) {
            continue;
        }
        if (!g_sched.shared_dirty_used[goal]) {
            return;
        }
    }
    reset_shared_batch();
}

void path_sched_reset_state(void) {
    g_sched.budget_ms = 1.5f;
    for (int i = 0; i < PATH_GOAL_COUNT; ++i) {
        clear_goal_state(&g_sched.goals[i], i);
    }
    reset_shared_batch();
}

void path_sched_shutdown_state(void) {
    for (int i = 0; i < PATH_GOAL_COUNT; ++i) {
        if (path_fields_is_building((PathGoal)i)) {
            path_fields_cancel_build((PathGoal)i);
        }
    }
    free(g_sched.shared_dirty_tiles);
    g_sched.shared_dirty_tiles = NULL;
    g_sched.shared_dirty_capacity = 0u;
    reset_shared_batch();
    path_sched_reset_state();
}

void path_sched_set_goal_data(PathGoal goal,
                              const HexWorld *world,
                              const int32_t *neighbors,
                              const TileId *goals,
                              const float *goal_seed_costs,
                              size_t goal_count,
                              size_t tile_count) {
    PathSchedGoalState *state = get_goal_state(goal);
    if (!state) {
        return;
    }
    if (path_fields_is_building(goal)) {
        path_fields_cancel_build(goal);
    }
    state->world = world;
    state->neighbors = neighbors;
    state->goals = goals;
    state->goal_count = goal_count;
    state->tile_count = tile_count;
    state->goal_seed_costs = goal_seed_costs;
    state->has_data = (world && neighbors && goals && goal_count > 0u && tile_count > 0u);
    state->building = false;
    state->pending_force = false;
    state->nodes_relaxed_accum = 0u;
    state->elapsed_ms_accum = 0.0;
    state->last_build_ms = 0.0f;
    state->last_relaxed = 0u;
    state->last_dirty_processed = 0u;
    state->dirty_seed_count_active = 0u;
    state->time_since_last_start_ms = 0.0;
    if (!state->has_data) {
        state->cadence_interval_ms = cadence_to_interval(state->cadence_hz);
    }
    if (goal >= 0 && goal < PATH_GOAL_COUNT) {
        g_sched.shared_dirty_used[goal] = false;
    }
}

static void requeue_shared_dirty(void) {
    if (!g_sched.shared_dirty_valid || g_sched.shared_dirty_count == 0u) {
        return;
    }
    path_cost_requeue_tiles(g_sched.shared_dirty_tiles, g_sched.shared_dirty_count);
    reset_shared_batch();
}

bool path_sched_update(float dt_sec, bool out_field_swapped[PATH_GOAL_COUNT]) {
    if (out_field_swapped) {
        for (int i = 0; i < PATH_GOAL_COUNT; ++i) {
            out_field_swapped[i] = false;
        }
    }

    double dt_ms = (dt_sec > 0.0f) ? (double)dt_sec * 1000.0 : 0.0;
    for (int goal = 0; goal < PATH_GOAL_COUNT; ++goal) {
        PathSchedGoalState *state = &g_sched.goals[goal];
        if (!state->building) {
            state->time_since_last_start_ms += dt_ms;
        }
    }

    double remaining_budget = g_sched.budget_ms;
    bool track_budget = (g_sched.budget_ms > 0.0f);

    for (int goal = 0; goal < PATH_GOAL_COUNT; ++goal) {
        PathSchedGoalState *state = &g_sched.goals[goal];
        if (!state->has_data) {
            continue;
        }

        double goal_budget = track_budget ? remaining_budget : g_sched.budget_ms;
        if (goal_budget < 0.0) {
            goal_budget = 0.0;
        }

        bool stepped = false;
        size_t relaxed = 0u;
        double step_ms = 0.0;
        bool finished = false;

        if (state->building) {
            if (path_fields_step((PathGoal)goal, goal_budget, &relaxed, &step_ms, &finished)) {
                state->nodes_relaxed_accum += relaxed;
                state->elapsed_ms_accum += step_ms;
                stepped = true;
            } else {
                LOG_WARN("path_sched: step failed for goal %d; canceling build", goal);
                state->building = false;
                state->nodes_relaxed_accum = 0u;
                state->elapsed_ms_accum = 0.0;
                state->dirty_seed_count_active = 0u;
                requeue_shared_dirty();
            }
        } else {
            bool has_dirty_batch = g_sched.shared_dirty_valid && !g_sched.shared_dirty_used[goal] &&
                                   g_sched.shared_dirty_count > 0u;
            if (!state->pending_force && !has_dirty_batch) {
                if (!g_sched.shared_dirty_valid && path_cost_dirty_count() > 0u) {
                    size_t limit = state->tile_count ? state->tile_count : path_cost_dirty_count();
                    if (ensure_shared_dirty_batch(limit)) {
                        has_dirty_batch = g_sched.shared_dirty_valid && !g_sched.shared_dirty_used[goal] &&
                                          g_sched.shared_dirty_count > 0u;
                    }
                }
            }

            bool cadence_due = (state->cadence_interval_ms <= 0.0) ||
                                (state->time_since_last_start_ms >= state->cadence_interval_ms);
            bool should_start = state->pending_force || has_dirty_batch || cadence_due;

            if (should_start) {
                const TileId *dirty_ptr = NULL;
                size_t dirty_count = 0u;
                if (!state->pending_force && g_sched.shared_dirty_valid && g_sched.shared_dirty_count > 0u &&
                    !g_sched.shared_dirty_used[goal]) {
                    dirty_ptr = g_sched.shared_dirty_tiles;
                    dirty_count = g_sched.shared_dirty_count;
                    g_sched.shared_dirty_used[goal] = true;
                }

                if (path_fields_start_build((PathGoal)goal,
                                            state->world,
                                            state->neighbors,
                                            state->goals,
                                            state->goal_count,
                                            state->goal_seed_costs,
                                            path_cost_eff_costs(),
                                            dirty_ptr,
                                            dirty_count)) {
                    state->building = true;
                    state->pending_force = false;
                    state->nodes_relaxed_accum = 0u;
                    state->elapsed_ms_accum = 0.0;
                    state->dirty_seed_count_active = dirty_count;
                    state->time_since_last_start_ms = 0.0;

                    if (path_fields_step((PathGoal)goal, goal_budget, &relaxed, &step_ms, &finished)) {
                        state->nodes_relaxed_accum += relaxed;
                        state->elapsed_ms_accum += step_ms;
                        stepped = true;
                    } else {
                        LOG_WARN("path_sched: step failed immediately for goal %d", goal);
                        state->building = false;
                        state->nodes_relaxed_accum = 0u;
                        state->elapsed_ms_accum = 0.0;
                        if (dirty_count > 0u) {
                            g_sched.shared_dirty_used[goal] = false;
                            requeue_shared_dirty();
                        }
                    }
                } else {
                    if (dirty_count > 0u) {
                        g_sched.shared_dirty_used[goal] = false;
                        requeue_shared_dirty();
                    }
                    state->pending_force = true;
                }
            }
        }

        if (stepped) {
            if (track_budget && step_ms > 0.0) {
                if (step_ms >= remaining_budget) {
                    remaining_budget = 0.0;
                } else {
                    remaining_budget -= step_ms;
                }
            }
            if (finished) {
                state->last_relaxed = state->nodes_relaxed_accum;
                state->last_build_ms = (float)state->elapsed_ms_accum;
                state->nodes_relaxed_accum = 0u;
                state->elapsed_ms_accum = 0.0;
                state->building = false;
                state->last_dirty_processed = state->dirty_seed_count_active;
                state->dirty_seed_count_active = 0u;
                state->time_since_last_start_ms = 0.0;
                if (out_field_swapped) {
                    out_field_swapped[goal] = true;
                }
            }
        }
    }

    finalize_shared_batch_if_consumed();
    return true;
}

void path_sched_set_budget_ms(float per_frame_ms) {
    if (per_frame_ms < 0.0f) {
        per_frame_ms = 0.0f;
    }
    g_sched.budget_ms = per_frame_ms;
}

void path_sched_set_cadence(PathGoal goal, float Hz) {
    PathSchedGoalState *state = get_goal_state(goal);
    if (!state) {
        return;
    }
    state->cadence_hz = (Hz > 0.0f) ? Hz : 0.0f;
    state->cadence_interval_ms = cadence_to_interval(state->cadence_hz);
    state->time_since_last_start_ms = 0.0;
}

float path_sched_get_last_build_ms(PathGoal goal) {
    PathSchedGoalState *state = get_goal_state(goal);
    if (!state) {
        return 0.0f;
    }
    return state->last_build_ms;
}

size_t path_sched_get_last_relaxed(PathGoal goal) {
    PathSchedGoalState *state = get_goal_state(goal);
    if (!state) {
        return 0u;
    }
    return state->last_relaxed;
}

bool path_sched_is_building(PathGoal goal) {
    PathSchedGoalState *state = get_goal_state(goal);
    if (!state) {
        return false;
    }
    return state->building;
}

uint32_t path_sched_get_stamp(PathGoal goal) {
    return path_field_stamp(goal);
}

void path_sched_force_full_recompute(PathGoal goal) {
    PathSchedGoalState *state = get_goal_state(goal);
    if (!state) {
        return;
    }
    state->pending_force = true;
}

size_t path_sched_get_dirty_queue_len(void) {
    size_t total = path_cost_dirty_count();
    if (g_sched.shared_dirty_valid) {
        total += g_sched.shared_dirty_count;
    }
    return total;
}

size_t path_sched_get_dirty_processed_last_build(PathGoal goal) {
    PathSchedGoalState *state = get_goal_state(goal);
    if (!state) {
        return 0u;
    }
    return state->last_dirty_processed;
}
