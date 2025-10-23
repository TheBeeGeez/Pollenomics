#ifndef TILE_GEN_H
#define TILE_GEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TileGenerationContext {
    int q;
    int r;
    uint64_t seed;
    float noise_primary;
    float noise_secondary;
    float biome_weight_meadow;
    float biome_weight_forest_edge;
    float distance_to_hive;
} TileGenerationContext;

#ifdef __cplusplus
}
#endif

#endif  // TILE_GEN_H
