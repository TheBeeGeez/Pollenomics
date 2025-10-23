#include "path/path_scheduler.h"

#include <stddef.h>
#include <stdlib.h>

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
    size_t nodes_relaxed_accum;
    double elapsed_ms_accum;
    float last_build_ms;
    size_t last_relaxed;
    TileId *dirty_tiles;
    size_t dirty_capacity;
    size_t current_dirty_seed_count;
    size_t last_dirty_processed;
} PathSchedGoalState;

typedef struct PathSchedulerState {
    float budget_ms;
    PathSchedGoalState goals[PATH_GOAL_COUNT];
} PathSchedulerState;

static PathSchedulerState g_sched = {0};

static const float kDefaultCadenceHz[PATH_GOAL_COUNT] = {10.0f};

static inline double cadence_to_interval(float hz) {
    return (hz > 0.0f) ? (1000.0 / (double)hz) : 0.0;
}

static PathSchedGoalState *get_goal_state(PathGoal goal) {
    if (goal < 0 || goal >= PATH_GOAL_COUNT) {
        return NULL;
    }
    return &g_sched.goals[goal];
}

static void clear_goal_state(PathSchedGoalState *state) {
    if (!state) {
        return;
    }
    free(state->dirty_tiles);
    state->dirty_tiles = NULL;
    state->dirty_capacity = 0u;
    state->current_dirty_seed_count = 0u;
    state->last_dirty_processed = 0u;
}

static bool ensure_dirty_capacity(PathSchedGoalState *state, size_t needed) {
    if (!state) {
        return false;
    }
    if (needed == 0u) {
        return true;
    }
    if (state->dirty_capacity >= needed) {
        return true;
    }
    size_t new_capacity = state->dirty_capacity ? state->dirty_capacity : 256u;
    while (new_capacity < needed) {
        new_capacity *= 2u;
    }
    TileId *tiles = (TileId *)realloc(state->dirty_tiles, new_capacity * sizeof(TileId));
    if (!tiles) {
        LOG_ERROR("path_sched: failed to grow dirty buffer (capacity=%zu)", new_capacity);
        return false;
    }
    state->dirty_tiles = tiles;
    state->dirty_capacity = new_capacity;
    return true;
}

void path_sched_reset_state(void) {
    g_sched.budget_ms = 1.5f;
    for (int i = 0; i < PATH_GOAL_COUNT; ++i) {
        PathSchedGoalState *state = &g_sched.goals[i];
        clear_goal_state(state);
        state->cadence_hz = (i < (int)(sizeof kDefaultCadenceHz / sizeof kDefaultCadenceHz[0]))
                                ? kDefaultCadenceHz[i]
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
        state->nodes_relaxed_accum = 0u;
        state->elapsed_ms_accum = 0.0;
        state->last_build_ms = 0.0f;
        state->last_relaxed = 0u;
        state->current_dirty_seed_count = 0u;
        state->last_dirty_processed = 0u;
    }
}

void path_sched_shutdown_state(void) {
    for (int i = 0; i < PATH_GOAL_COUNT; ++i) {
        if (path_fields_is_building((PathGoal)i)) {
            path_fields_cancel_build((PathGoal)i);
        }
        clear_goal_state(&g_sched.goals[i]);
    }
    path_sched_reset_state();
}

void path_sched_set_goal_data(PathGoal goal,
                              const HexWorld *world,
                              const int32_t *neighbors,
                              const TileId *goals,
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
    state->has_data = (world && neighbors && goals && goal_count > 0u && tile_count > 0u);
    state->building = false;
    state->pending_force = false;
    state->nodes_relaxed_accum = 0u;
    state->elapsed_ms_accum = 0.0;
    state->time_since_last_start_ms = 0.0;
    if (state->has_data) {
        ensure_dirty_capacity(state, tile_count);
    }
}

bool path_sched_update(float dt_sec, bool *out_field_swapped) {
    if (out_field_swapped) {
        *out_field_swapped = false;
    }
    PathSchedGoalState *state = get_goal_state(PATH_GOAL_ENTRANCE);
    if (!state) {
        return false;
    }
    double dt_ms = (dt_sec > 0.0f) ? (double)dt_sec * 1000.0 : 0.0;
    if (!state->building) {
        state->time_since_last_start_ms += dt_ms;
    }

    size_t dirty_queue_len = path_cost_dirty_count();
    bool swapped = false;
    if (state->building) {
        size_t relaxed = 0u;
        double step_ms = 0.0;
        bool finished = false;
        if (path_fields_step(PATH_GOAL_ENTRANCE, g_sched.budget_ms, &relaxed, &step_ms, &finished)) {
            state->nodes_relaxed_accum += relaxed;
            state->elapsed_ms_accum += step_ms;
            if (finished) {
                state->last_relaxed = state->nodes_relaxed_accum;
                state->last_build_ms = (float)state->elapsed_ms_accum;
                state->nodes_relaxed_accum = 0u;
                state->elapsed_ms_accum = 0.0;
                state->building = false;
                state->time_since_last_start_ms = 0.0;
                state->last_dirty_processed = state->current_dirty_seed_count;
                state->current_dirty_seed_count = 0u;
                swapped = true;
            }
        } else {
            LOG_WARN("path_sched: step failed; canceling build");
            state->building = false;
            state->nodes_relaxed_accum = 0u;
            state->elapsed_ms_accum = 0.0;
            if (state->current_dirty_seed_count > 0u) {
                path_cost_requeue_tiles(state->dirty_tiles, state->current_dirty_seed_count);
                state->current_dirty_seed_count = 0u;
            }
        }
    } else {
        bool should_start = state->pending_force;
        if (!should_start) {
            if (!state->has_data) {
                state->time_since_last_start_ms = 0.0;
            } else if (dirty_queue_len > 0u) {
                should_start = true;
            } else if (state->cadence_interval_ms <= 0.0) {
                should_start = true;
            } else if (state->time_since_last_start_ms >= state->cadence_interval_ms) {
                should_start = true;
            }
        }
        if (should_start && state->has_data) {
            size_t dirty_to_seed = dirty_queue_len;
            if (dirty_to_seed > state->tile_count) {
                dirty_to_seed = state->tile_count;
            }
            if (ensure_dirty_capacity(state, dirty_to_seed > 0u ? dirty_to_seed : 1u)) {
                size_t consumed = 0u;
                if (dirty_to_seed > 0u) {
                    consumed = path_cost_consume_dirty(state->dirty_tiles, dirty_to_seed);
                }
                const TileId *dirty_ptr = (consumed > 0u) ? state->dirty_tiles : NULL;
                state->current_dirty_seed_count = consumed;
                if (path_fields_start_build(PATH_GOAL_ENTRANCE,
                                            state->world,
                                            state->neighbors,
                                            state->goals,
                                            state->goal_count,
                                            path_cost_eff_costs(),
                                            dirty_ptr,
                                            consumed)) {
                    state->building = true;
                    state->nodes_relaxed_accum = 0u;
                    state->elapsed_ms_accum = 0.0;
                    state->time_since_last_start_ms = 0.0;
                    state->pending_force = false;
                    size_t relaxed = 0u;
                    double step_ms = 0.0;
                    bool finished = false;
                    if (path_fields_step(PATH_GOAL_ENTRANCE,
                                          g_sched.budget_ms,
                                          &relaxed,
                                          &step_ms,
                                          &finished)) {
                        state->nodes_relaxed_accum += relaxed;
                        state->elapsed_ms_accum += step_ms;
                        if (finished) {
                            state->last_relaxed = state->nodes_relaxed_accum;
                            state->last_build_ms = (float)state->elapsed_ms_accum;
                            state->nodes_relaxed_accum = 0u;
                            state->elapsed_ms_accum = 0.0;
                            state->building = false;
                            state->last_dirty_processed = state->current_dirty_seed_count;
                            state->current_dirty_seed_count = 0u;
                            swapped = true;
                        }
                    } else {
                        LOG_WARN("path_sched: step failed immediately after start");
                        state->building = false;
                        state->nodes_relaxed_accum = 0u;
                        state->elapsed_ms_accum = 0.0;
                        if (state->current_dirty_seed_count > 0u) {
                            path_cost_requeue_tiles(state->dirty_tiles, state->current_dirty_seed_count);
                            state->current_dirty_seed_count = 0u;
                        }
                    }
                } else {
                    if (consumed > 0u) {
                        path_cost_requeue_tiles(state->dirty_tiles, consumed);
                    }
                    state->current_dirty_seed_count = 0u;
                    state->pending_force = true;
                }
            } else {
                LOG_WARN("path_sched: unable to reserve dirty buffer; deferring build");
                state->pending_force = true;
                state->time_since_last_start_ms = 0.0;
            }
        }
    }

    if (out_field_swapped) {
        *out_field_swapped = swapped;
    }
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
    return path_cost_dirty_count();
}

size_t path_sched_get_dirty_processed_last_build(PathGoal goal) {
    PathSchedGoalState *state = get_goal_state(goal);
    if (!state) {
        return 0u;
    }
    return state->last_dirty_processed;
}
