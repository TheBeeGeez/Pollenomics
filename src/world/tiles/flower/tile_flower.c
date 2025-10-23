#include "world/tiles/tile_flower.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "hex.h"
#include "util/log.h"

#define INVALID_PAYLOAD_INDEX 0xFFFFFFFFu

typedef enum FlowerArchetypeId {
    FLOWER_ARCHETYPE_CLOVER = 0,
    FLOWER_ARCHETYPE_WILDFLOWER = 1,
    FLOWER_ARCHETYPE_ORCHARD = 2,
    FLOWER_ARCHETYPE_ROADSIDE = 3,
    FLOWER_ARCHETYPE_COUNT
} FlowerArchetypeId;

static uint32_t pack_rgba(float r, float g, float b, float a) {
    uint32_t ri = (uint32_t)(fminf(fmaxf(r, 0.0f), 1.0f) * 255.0f + 0.5f);
    uint32_t gi = (uint32_t)(fminf(fmaxf(g, 0.0f), 1.0f) * 255.0f + 0.5f);
    uint32_t bi = (uint32_t)(fminf(fmaxf(b, 0.0f), 1.0f) * 255.0f + 0.5f);
    uint32_t ai = (uint32_t)(fminf(fmaxf(a, 0.0f), 1.0f) * 255.0f + 0.5f);
    return (ri << 24) | (gi << 16) | (bi << 8) | ai;
}

static const FlowerArchetype k_archetypes[FLOWER_ARCHETYPE_COUNT] = {
    [FLOWER_ARCHETYPE_CLOVER] = {
        .name = "Clover Meadow",
        .capacity = 220.0f,
        .initial_fill = 0.85f,
        .recharge_rate = 18.0f,
        .recharge_multiplier_day = 1.15f,
        .recharge_multiplier_night = 0.25f,
        .quality = 0.68f,
        .viscosity = 0.85f,
        .color_rgba = 0xF080C2C7u,
    },
    [FLOWER_ARCHETYPE_WILDFLOWER] = {
        .name = "Wildflower Mix",
        .capacity = 160.0f,
        .initial_fill = 0.70f,
        .recharge_rate = 12.0f,
        .recharge_multiplier_day = 0.95f,
        .recharge_multiplier_night = 0.30f,
        .quality = 0.74f,
        .viscosity = 0.95f,
        .color_rgba = 0xEB70A8CCu,
    },
    [FLOWER_ARCHETYPE_ORCHARD] = {
        .name = "Orchard Bloom",
        .capacity = 260.0f,
        .initial_fill = 0.60f,
        .recharge_rate = 10.0f,
        .recharge_multiplier_day = 1.35f,
        .recharge_multiplier_night = 0.20f,
        .quality = 0.92f,
        .viscosity = 1.05f,
        .color_rgba = 0xFA8FC7D9u,
    },
    [FLOWER_ARCHETYPE_ROADSIDE] = {
        .name = "Roadside Weeds",
        .capacity = 90.0f,
        .initial_fill = 0.55f,
        .recharge_rate = 8.5f,
        .recharge_multiplier_day = 0.80f,
        .recharge_multiplier_night = 0.45f,
        .quality = 0.38f,
        .viscosity = 0.65f,
        .color_rgba = 0xE05CB3B3u,
    },
};

static float rand_uniform01(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    uint32_t hi = (uint32_t)(x >> 32);
    return (float)hi / 4294967295.0f;
}

static FlowerArchetypeId pick_archetype(uint64_t *seed, int q, int r) {
    uint64_t mix = ((uint64_t)(uint32_t)q << 32) ^ (uint64_t)(uint32_t)r ^ (*seed + 0x9E3779B97F4A7C15ULL);
    float primary = rand_uniform01(&mix);
    if (primary < 0.35f) {
        return FLOWER_ARCHETYPE_CLOVER;
    }
    if (primary < 0.68f) {
        return FLOWER_ARCHETYPE_WILDFLOWER;
    }
    if (primary < 0.88f) {
        return FLOWER_ARCHETYPE_ORCHARD;
    }
    return FLOWER_ARCHETYPE_ROADSIDE;
}

static bool ensure_capacity_generic(void **buffer, size_t element_size, size_t *capacity, size_t required) {
    if (!buffer || !capacity) {
        return false;
    }
    if (*capacity >= required) {
        return true;
    }
    size_t new_capacity = (*capacity == 0) ? required : *capacity;
    while (new_capacity < required) {
        new_capacity = new_capacity * 2 + 1;
    }
    void *new_buffer = realloc(*buffer, new_capacity * element_size);
    if (!new_buffer) {
        return false;
    }
    *buffer = new_buffer;
    *capacity = new_capacity;
    return true;
}

static void flower_bind_payload(FlowerSystem *system, size_t tile_index, uint32_t payload_index) {
    if (!system || tile_index >= system->tile_to_payload_capacity) {
        return;
    }
    system->tile_to_payload[tile_index] = payload_index;
}

static FlowerPayload *flower_payload_for_tile(FlowerSystem *system, size_t tile_index) {
    if (!system || tile_index >= system->tile_to_payload_capacity) {
        return NULL;
    }
    uint32_t payload_index = system->tile_to_payload[tile_index];
    if (payload_index == INVALID_PAYLOAD_INDEX || payload_index >= system->payload_count) {
        return NULL;
    }
    return &system->payloads[payload_index];
}

static const FlowerPayload *flower_payload_for_tile_const(const FlowerSystem *system, size_t tile_index) {
    if (!system || tile_index >= system->tile_to_payload_capacity) {
        return NULL;
    }
    uint32_t payload_index = system->tile_to_payload[tile_index];
    if (payload_index == INVALID_PAYLOAD_INDEX || payload_index >= system->payload_count) {
        return NULL;
    }
    return &system->payloads[payload_index];
}

void tile_flower_system_init(FlowerSystem *system) {
    if (!system) {
        return;
    }
    memset(system, 0, sizeof(*system));
}

void tile_flower_system_shutdown(FlowerSystem *system) {
    if (!system) {
        return;
    }
    free(system->payloads);
    free(system->tile_indices);
    free(system->tile_to_payload);
    memset(system, 0, sizeof(*system));
}

void tile_flower_system_reset(FlowerSystem *system, size_t tile_capacity) {
    if (!system) {
        return;
    }
    system->payload_count = 0;
    system->tile_index_count = 0;
    if (!ensure_capacity_generic((void **)&system->payloads, sizeof(FlowerPayload), &system->payload_capacity, tile_capacity)) {
        LOG_ERROR("flower: failed to reserve %zu payload slots", tile_capacity);
    }
    if (!ensure_capacity_generic((void **)&system->tile_indices, sizeof(size_t), &system->tile_index_capacity, tile_capacity)) {
        LOG_ERROR("flower: failed to reserve %zu tile indices", tile_capacity);
    }
    if (!ensure_capacity_generic((void **)&system->tile_to_payload, sizeof(uint32_t), &system->tile_to_payload_capacity,
                                 tile_capacity)) {
        LOG_ERROR("flower: failed to reserve %zu tile map entries", tile_capacity);
    }
    if (system->tile_to_payload) {
        memset(system->tile_to_payload, 0xFF, system->tile_to_payload_capacity * sizeof(uint32_t));
    }
}

static void flower_on_world_reset(void *user_data, HexWorld *world, size_t tile_capacity) {
    (void)world;
    FlowerSystem *system = (FlowerSystem *)user_data;
    tile_flower_system_reset(system, tile_capacity);
}

static void flower_generate_tile(void *user_data, HexWorld *world, TileId id, int q, int r, uint64_t rng_seed) {
    FlowerSystem *system = (FlowerSystem *)user_data;
    if (!system || !world || id >= world->tile_count) {
        return;
    }
    FlowerArchetypeId archetype_id = pick_archetype(&rng_seed, q, r);
    const FlowerArchetype *archetype = &k_archetypes[archetype_id];

    if (!ensure_capacity_generic((void **)&system->payloads, sizeof(FlowerPayload), &system->payload_capacity,
                                 system->payload_count + 1)) {
        return;
    }
    if (!ensure_capacity_generic((void **)&system->tile_indices, sizeof(size_t), &system->tile_index_capacity,
                                 system->tile_index_count + 1)) {
        return;
    }
    if (id >= system->tile_to_payload_capacity) {
        size_t previous = system->tile_to_payload_capacity;
        if (!ensure_capacity_generic((void **)&system->tile_to_payload, sizeof(uint32_t), &system->tile_to_payload_capacity,
                                     id + 1)) {
            return;
        }
        if (system->tile_to_payload && system->tile_to_payload_capacity > previous) {
            memset(system->tile_to_payload + previous, 0xFF,
                   (system->tile_to_payload_capacity - previous) * sizeof(uint32_t));
        }
    }

    uint32_t payload_index = (uint32_t)system->payload_count;
    system->payloads[payload_index] = (FlowerPayload){
        .archetype_id = (uint16_t)archetype_id,
        .capacity = archetype->capacity,
        .stock = archetype->capacity * archetype->initial_fill,
        .recharge_rate = archetype->recharge_rate,
        .recharge_multiplier = archetype->recharge_multiplier_day,
        .quality = archetype->quality,
        .viscosity = archetype->viscosity,
    };
    system->payload_count++;

    system->tile_indices[system->tile_index_count++] = id;
    flower_bind_payload(system, id, payload_index);

    HexTile *tile = &world->tiles[id];
    tile->terrain = HEX_TERRAIN_FLOWERS;
    tile->nectar_capacity = system->payloads[payload_index].capacity;
    tile->nectar_stock = system->payloads[payload_index].stock;
    tile->nectar_recharge_rate = system->payloads[payload_index].recharge_rate;
    tile->nectar_recharge_multiplier = system->payloads[payload_index].recharge_multiplier;
    tile->flower_quality = system->payloads[payload_index].quality;
    tile->flower_viscosity = system->payloads[payload_index].viscosity;
    tile->patch_id = -1;
    tile->flow_capacity = 18.0f;
    tile->flower_archetype_id = system->payloads[payload_index].archetype_id;
}

static bool flower_populate_info(void *user_data, const HexWorld *world, TileId id, TileInfo *out_info) {
    (void)world;
    const FlowerSystem *system = (const FlowerSystem *)user_data;
    if (!system || !out_info) {
        return false;
    }
    const FlowerPayload *payload = flower_payload_for_tile_const(system, id);
    if (!payload) {
        return false;
    }
    out_info->terrain = TILE_TERRAIN_FLOWERS;
    out_info->nectar_capacity = payload->capacity;
    out_info->nectar_stock = payload->stock;
    out_info->nectar_recharge_rate = payload->recharge_rate;
    out_info->nectar_recharge_multiplier = payload->recharge_multiplier;
    out_info->flower_quality = payload->quality;
    out_info->flower_viscosity = payload->viscosity;
    out_info->flow_capacity = 18.0f;
    out_info->patch_id = -1;
    out_info->archetype_id = payload->archetype_id;
    return true;
}

static float flower_harvest(void *user_data, HexWorld *world, TileId id, float request_uL, float *quality_out) {
    FlowerSystem *system = (FlowerSystem *)user_data;
    if (!system || !world || id >= world->tile_count) {
        return 0.0f;
    }
    if (request_uL <= 0.0f) {
        const FlowerPayload *payload_const = flower_payload_for_tile_const(system, id);
        if (quality_out && payload_const) {
            *quality_out = payload_const->quality;
        }
        return 0.0f;
    }
    FlowerPayload *payload = flower_payload_for_tile(system, id);
    if (!payload) {
        return 0.0f;
    }
    float harvest = request_uL;
    if (harvest > payload->stock) {
        harvest = payload->stock;
    }
    payload->stock -= harvest;
    HexTile *tile = &world->tiles[id];
    tile->nectar_stock = payload->stock;
    if (quality_out) {
        *quality_out = payload->quality;
    }
    return harvest;
}

static bool flower_is_floral(void *user_data, TileId id) {
    const FlowerSystem *system = (const FlowerSystem *)user_data;
    if (!system || id >= system->tile_to_payload_capacity) {
        return false;
    }
    uint32_t payload_index = system->tile_to_payload[id];
    return payload_index != INVALID_PAYLOAD_INDEX && payload_index < system->payload_count;
}

static void flower_apply_palette(void *user_data, HexWorld *world, bool nectar_heatmap_enabled) {
    FlowerSystem *system = (FlowerSystem *)user_data;
    if (!system || !world || !world->fill_rgba) {
        return;
    }
    for (size_t i = 0; i < system->tile_index_count; ++i) {
        size_t tile_index = system->tile_indices[i];
        if (tile_index >= world->tile_count) {
            continue;
        }
        const FlowerPayload *payload = flower_payload_for_tile_const(system, tile_index);
        if (!payload) {
            continue;
        }
        uint16_t archetype_id = payload->archetype_id;
        if (archetype_id < FLOWER_ARCHETYPE_COUNT) {
            world->fill_rgba[tile_index] = k_archetypes[archetype_id].color_rgba;
        }
        if (nectar_heatmap_enabled && payload->capacity > 0.0f) {
            float ratio = payload->stock / payload->capacity;
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;
            float brightness = 0.25f + 0.75f * ratio;
            uint32_t base = world->fill_rgba[tile_index];
            float r = ((float)((base >> 24) & 0xFFu)) / 255.0f;
            float g = ((float)((base >> 16) & 0xFFu)) / 255.0f;
            float b = ((float)((base >> 8) & 0xFFu)) / 255.0f;
            float a = ((float)(base & 0xFFu)) / 255.0f;
            r *= brightness;
            g *= brightness;
            b *= brightness;
            world->fill_rgba[tile_index] = pack_rgba(r, g, b, a);
        }
    }
}

static TileTypeVTable g_flower_vtable = {
    .name = "Flowers",
    .on_world_reset = flower_on_world_reset,
    .generate_tile = flower_generate_tile,
    .tick = NULL,
    .populate_info = flower_populate_info,
    .harvest = flower_harvest,
    .is_floral = flower_is_floral,
    .apply_palette = flower_apply_palette,
};

void tile_flower_register(TileRegistry *registry, FlowerSystem *system) {
    if (!registry || !system) {
        return;
    }
    tile_registry_register(registry, TILE_TERRAIN_FLOWERS, &g_flower_vtable, system);
}

bool tile_flower_tile_info(const FlowerSystem *system, size_t tile_index, TileInfo *out_info) {
    return flower_populate_info((void *)system, NULL, tile_index, out_info);
}

float tile_flower_harvest(FlowerSystem *system,
                          HexWorld *world,
                          size_t tile_index,
                          float request_uL,
                          float *quality_out) {
    return flower_harvest(system, world, tile_index, request_uL, quality_out);
}

void tile_flower_tick(FlowerSystem *system, HexWorld *world, float dt_sec) {
    if (!system || !world || dt_sec <= 0.0f) {
        return;
    }
    for (size_t i = 0; i < system->tile_index_count; ++i) {
        size_t tile_index = system->tile_indices[i];
        if (tile_index >= world->tile_count) {
            continue;
        }
        FlowerPayload *payload = flower_payload_for_tile(system, tile_index);
        if (!payload) {
            continue;
        }
        float recharge = payload->recharge_rate * payload->recharge_multiplier * dt_sec;
        payload->stock += recharge;
        if (payload->stock > payload->capacity) {
            payload->stock = payload->capacity;
        }
        if (payload->stock < 0.0f) {
            payload->stock = 0.0f;
        }
        HexTile *tile = &world->tiles[tile_index];
        tile->nectar_stock = payload->stock;
        tile->nectar_recharge_rate = payload->recharge_rate;
        tile->nectar_recharge_multiplier = payload->recharge_multiplier;
        tile->flower_quality = payload->quality;
        tile->flower_viscosity = payload->viscosity;
    }
}

uint32_t tile_flower_color(const FlowerSystem *system, size_t tile_index, uint32_t fallback_rgba) {
    const FlowerPayload *payload = flower_payload_for_tile_const(system, tile_index);
    if (!payload) {
        return fallback_rgba;
    }
    uint16_t archetype_id = payload->archetype_id;
    if (archetype_id < FLOWER_ARCHETYPE_COUNT) {
        return k_archetypes[archetype_id].color_rgba;
    }
    return fallback_rgba;
}

const char *tile_flower_archetype_name(const FlowerSystem *system, size_t tile_index) {
    const FlowerPayload *payload = flower_payload_for_tile_const(system, tile_index);
    if (!payload) {
        return NULL;
    }
    uint16_t archetype_id = payload->archetype_id;
    if (archetype_id < FLOWER_ARCHETYPE_COUNT) {
        return k_archetypes[archetype_id].name;
    }
    return NULL;
}

bool tile_flower_override_payload(FlowerSystem *system,
                                  HexWorld *world,
                                  size_t tile_index,
                                  float capacity,
                                  float stock,
                                  float recharge_rate,
                                  float recharge_multiplier,
                                  float quality,
                                  float viscosity) {
    if (!system || !world || tile_index >= world->tile_count) {
        return false;
    }
    FlowerPayload *payload = flower_payload_for_tile(system, tile_index);
    if (!payload) {
        return false;
    }
    if (capacity < 0.0f) capacity = 0.0f;
    if (stock < 0.0f) stock = 0.0f;
    if (capacity > 0.0f && stock > capacity) {
        stock = capacity;
    }
    if (quality < 0.0f) quality = 0.0f;
    if (quality > 1.0f) quality = 1.0f;
    if (viscosity <= 0.0f) viscosity = 1.0f;

    payload->capacity = capacity;
    payload->stock = stock;
    payload->recharge_rate = recharge_rate >= 0.0f ? recharge_rate : 0.0f;
    payload->recharge_multiplier = recharge_multiplier;
    payload->quality = quality;
    payload->viscosity = viscosity;

    HexTile *tile = &world->tiles[tile_index];
    tile->terrain = HEX_TERRAIN_FLOWERS;
    tile->nectar_capacity = payload->capacity;
    tile->nectar_stock = payload->stock;
    tile->nectar_recharge_rate = payload->recharge_rate;
    tile->nectar_recharge_multiplier = recharge_multiplier;
    tile->flower_quality = payload->quality;
    tile->flower_viscosity = payload->viscosity;
    return true;
}
