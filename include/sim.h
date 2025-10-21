#ifndef SIM_H
#define SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bee.h"
#include "params.h"
#include "render.h"

typedef struct SimState SimState;

typedef struct SimInit {
    const Params *params;      // Optional external params pointer.
    size_t capacity_override;  // Future: allow manual capacity specification.
} SimInit;

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

size_t sim_find_bee_near(const SimState *state, float world_x, float world_y, float radius_world);
// Returns the index of the closest bee within radius_world (inclusive), or SIZE_MAX when none.

bool sim_get_bee_info(const SimState *state, size_t index, BeeDebugInfo *out_info);
// Populates BeeDebugInfo for the given index; returns false if out of range.

#endif  // SIM_H
