#ifndef TILE_CORE_H
#define TILE_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef size_t TileId;

typedef enum TileTerrain {
    TILE_TERRAIN_OPEN = 0,
    TILE_TERRAIN_FOREST = 1,
    TILE_TERRAIN_MOUNTAIN = 2,
    TILE_TERRAIN_WATER = 3,
    TILE_TERRAIN_HIVE = 4,
    TILE_TERRAIN_FLOWERS = 5,
    TILE_TERRAIN_ENTRANCE = 6,
    TILE_TERRAIN_COUNT
} TileTerrain;

// Temporary aliases to maintain compatibility with existing HexTerrain usage.
// These ensure that legacy code that still refers to HEX_TERRAIN_* continues to
// compile while the broader refactor migrates to the new TileTerrain names.
#define HEX_TERRAIN_OPEN TILE_TERRAIN_OPEN
#define HEX_TERRAIN_FOREST TILE_TERRAIN_FOREST
#define HEX_TERRAIN_MOUNTAIN TILE_TERRAIN_MOUNTAIN
#define HEX_TERRAIN_WATER TILE_TERRAIN_WATER
#define HEX_TERRAIN_HIVE TILE_TERRAIN_HIVE
#define HEX_TERRAIN_FLOWERS TILE_TERRAIN_FLOWERS
#define HEX_TERRAIN_ENTRANCE TILE_TERRAIN_ENTRANCE
#define HEX_TERRAIN_COUNT TILE_TERRAIN_COUNT

typedef struct TileInfo {
    TileTerrain terrain;
    float nectar_stock;
    float nectar_capacity;
    float nectar_recharge_rate;
    float nectar_recharge_multiplier;
    float flower_quality;
    float flower_viscosity;
    float flow_capacity;
    int16_t patch_id;
    uint16_t archetype_id;
} TileInfo;

struct HexWorld;

typedef struct TileTypeVTable {
    const char *name;
    void (*on_world_reset)(void *user_data, struct HexWorld *world, size_t tile_capacity);
    void (*generate_tile)(void *user_data,
                          struct HexWorld *world,
                          TileId id,
                          int q,
                          int r,
                          uint64_t rng_seed);
    void (*tick)(void *user_data, struct HexWorld *world, float dt_sec);
    bool (*populate_info)(void *user_data, const struct HexWorld *world, TileId id, TileInfo *out_info);
    float (*harvest)(void *user_data,
                     struct HexWorld *world,
                     TileId id,
                     float request_uL,
                     float *quality_out);
    bool (*is_floral)(void *user_data, TileId id);
    void (*apply_palette)(void *user_data, struct HexWorld *world, bool nectar_heatmap_enabled);
} TileTypeVTable;

typedef struct TileTypeRegistration {
    bool in_use;
    TileTerrain terrain;
    const TileTypeVTable *vtable;
    void *user_data;
} TileTypeRegistration;

typedef struct TileRegistry {
    TileTypeRegistration entries[TILE_TERRAIN_COUNT];
} TileRegistry;

void tile_registry_init(TileRegistry *registry);
bool tile_registry_register(TileRegistry *registry,
                            TileTerrain terrain,
                            const TileTypeVTable *vtable,
                            void *user_data);
const TileTypeRegistration *tile_registry_get(const TileRegistry *registry, TileTerrain terrain);

#ifdef __cplusplus
}
#endif

#endif  // TILE_CORE_H
