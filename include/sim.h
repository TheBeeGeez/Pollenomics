#ifndef SIM_H
#define SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "params.h"
#include "render.h"

typedef struct SimState SimState;

typedef struct SimInit {
    const Params *params;      // Optional external params pointer.
    size_t capacity_override;  // Future: allow manual capacity specification.
} SimInit;

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

bool sim_init(SimState **out_state, const Params *params);
// Allocates and initializes the simulation buffers using Params. Returns false
// on allocation failure or invalid arguments, leaving *out_state untouched.

void sim_tick(SimState *state, float dt_sec);
// Advances the simulation by dt_sec seconds. No allocations occur here.

RenderView sim_build_view(const SimState *state);
// Builds a renderable view over the simulation buffers. Pointers remain valid
// until the next call to sim_tick or sim_reset.

void sim_reset(SimState *state, uint64_t seed);
// Reinitializes the simulation deterministically from the given seed.

void sim_apply_runtime_params(SimState *state, const Params *params);
// Updates motion-related tunables in-place without reallocating or
// reseeding. Positions and velocities are clamped to remain valid.

void sim_shutdown(SimState *state);
// Frees all simulation resources; safe to call on null.

#endif  // SIM_H
