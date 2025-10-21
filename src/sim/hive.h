#ifndef SIM_HIVE_H
#define SIM_HIVE_H

#include "sim_internal.h"

void hive_build_segments(SimState *state);
void hive_resolve_disc(const SimState *state, float radius, float *x, float *y, float *vx, float *vy);
void hive_compute_points(const SimState *state, float *entrance_x, float *entrance_y, float *unload_x, float *unload_y);

#endif  // SIM_HIVE_H
