#include "tile_core.h"

#include <string.h>

void tile_registry_init(TileRegistry *registry) {
    if (!registry) {
        return;
    }
    memset(registry, 0, sizeof(*registry));
}

bool tile_registry_register(TileRegistry *registry,
                            TileTerrain terrain,
                            const TileTypeVTable *vtable,
                            void *user_data) {
    if (!registry || terrain < 0 || terrain >= TILE_TERRAIN_COUNT || !vtable) {
        return false;
    }
    TileTypeRegistration *entry = &registry->entries[terrain];
    if (entry->in_use) {
        return false;
    }
    entry->in_use = true;
    entry->terrain = terrain;
    entry->vtable = vtable;
    entry->user_data = user_data;
    return true;
}

const TileTypeRegistration *tile_registry_get(const TileRegistry *registry, TileTerrain terrain) {
    if (!registry || terrain < 0 || terrain >= TILE_TERRAIN_COUNT) {
        return NULL;
    }
    const TileTypeRegistration *entry = &registry->entries[terrain];
    return entry->in_use ? entry : NULL;
}
