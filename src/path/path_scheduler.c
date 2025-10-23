#include "path/path_scheduler.h"

#include <stddef.h>

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

void path_sched_reset_state(void) {
    g_sched.budget_ms = 1.5f;
    for (int i = 0; i < PATH_GOAL_COUNT; ++i) {
        PathSchedGoalState *state = &g_sched.goals[i];
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
    }
}

void path_sched_shutdown_state(void) {
    for (int i = 0; i < PATH_GOAL_COUNT; ++i) {
        if (path_fields_is_building((PathGoal)i)) {
            path_fields_cancel_build((PathGoal)i);
        }
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
                swapped = true;
            }
        } else {
            LOG_WARN("path_sched: step failed; canceling build");
            state->building = false;
            state->nodes_relaxed_accum = 0u;
            state->elapsed_ms_accum = 0.0;
        }
    } else {
        bool should_start = state->pending_force;
        if (!should_start) {
            if (!state->has_data) {
                state->time_since_last_start_ms = 0.0;
            } else if (state->cadence_interval_ms <= 0.0) {
                should_start = true;
            } else if (state->time_since_last_start_ms >= state->cadence_interval_ms) {
                should_start = true;
            }
        }
        if (should_start && state->has_data) {
            if (path_fields_start_build(PATH_GOAL_ENTRANCE,
                                        state->world,
                                        state->neighbors,
                                        state->goals,
                                        state->goal_count)) {
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
                        swapped = true;
                    }
                } else {
                    LOG_WARN("path_sched: step failed immediately after start");
                    state->building = false;
                    state->nodes_relaxed_accum = 0u;
                    state->elapsed_ms_accum = 0.0;
                }
            } else {
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
    if (!state->has_data) {
        return;
    }
    if (path_fields_start_build(goal, state->world, state->neighbors, state->goals, state->goal_count)) {
        state->building = true;
        state->nodes_relaxed_accum = 0u;
        state->elapsed_ms_accum = 0.0;
        state->time_since_last_start_ms = 0.0;
        state->pending_force = false;
    }
}
