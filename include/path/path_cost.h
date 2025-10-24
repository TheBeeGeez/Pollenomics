#ifndef PATH_PATH_COST_H
#define PATH_PATH_COST_H

#include <stddef.h>

#include "tile_core.h"

void path_cost_set_coeffs(float alpha_congestion, float gamma_hazard);
void path_cost_set_ema_lambda(float lambda);
void path_cost_set_dirty_threshold(float relative_eps);
void path_cost_set_hazard(TileId nid, float penalty);
void path_cost_add_crowd_samples(const TileId *tiles, const float *bees_per_sec, int count);
void path_cost_mark_dirty(TileId nid);
void path_cost_mark_many_dirty(const TileId *tiles, int count);

#endif  // PATH_PATH_COST_H
