#ifndef TILE_TYPES_H
#define TILE_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FlowerArchetype {
    const char *name;
    float capacity;
    float initial_fill;
    float recharge_rate;
    float recharge_multiplier_day;
    float recharge_multiplier_night;
    float quality;
    float viscosity;
    uint32_t color_rgba;
} FlowerArchetype;

typedef struct FlowerPayload {
    uint16_t archetype_id;
    float capacity;
    float stock;
    float recharge_rate;
    float recharge_multiplier;
    float quality;
    float viscosity;
} FlowerPayload;

#ifdef __cplusplus
}
#endif

#endif  // TILE_TYPES_H
