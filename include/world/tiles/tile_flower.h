#ifndef TILE_FLOWER_H
#define TILE_FLOWER_H

#include "tile_core.h"
#include "tile_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FlowerSystem {
    FlowerPayload *payloads;
    size_t payload_count;
    size_t payload_capacity;
    size_t *tile_indices;
    size_t tile_index_count;
    size_t tile_index_capacity;
    uint32_t *tile_to_payload;
    size_t tile_to_payload_capacity;
} FlowerSystem;

void tile_flower_system_init(FlowerSystem *system);
void tile_flower_system_shutdown(FlowerSystem *system);
void tile_flower_system_reset(FlowerSystem *system, size_t tile_capacity);

void tile_flower_register(TileRegistry *registry, FlowerSystem *system);

bool tile_flower_tile_info(const FlowerSystem *system, size_t tile_index, TileInfo *out_info);
float tile_flower_harvest(FlowerSystem *system,
                          struct HexWorld *world,
                          size_t tile_index,
                          float request_uL,
                          float *quality_out);
void tile_flower_tick(FlowerSystem *system, struct HexWorld *world, float dt_sec);
uint32_t tile_flower_color(const FlowerSystem *system, size_t tile_index, uint32_t fallback_rgba);
const char *tile_flower_archetype_name(const FlowerSystem *system, size_t tile_index);
bool tile_flower_override_payload(FlowerSystem *system,
                                  struct HexWorld *world,
                                  size_t tile_index,
                                  float capacity,
                                  float stock,
                                  float recharge_rate,
                                  float recharge_multiplier,
                                  float quality,
                                  float viscosity);

#ifdef __cplusplus
}
#endif

#endif  // TILE_FLOWER_H
