#ifndef SIM_PLANTS_H
#define SIM_PLANTS_H

#include "sim_internal.h"

void plants_generate(SimState *state, uint64_t *rng_state);
void plants_replenish(SimState *state, float dt_sec);
int32_t plants_choose_patch(const SimState *state, float from_x, float from_y, uint64_t *rng);
void plants_sample_point(const FlowerPatch *patch, uint64_t *rng, float *out_x, float *out_y);
FlowerPatch *plants_get_patch(SimState *state, int32_t patch_id);
const FlowerPatch *plants_get_patch_const(const SimState *state, int32_t patch_id);

#endif  // SIM_PLANTS_H
