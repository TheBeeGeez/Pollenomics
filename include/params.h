#ifndef PARAMS_H
#define PARAMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Params holds immutable configuration values supplied at boot.
// Invariants enforced by params_validate: window dimensions >= safe minimums,
// window title non-empty, sensible render/sim defaults. No runtime state or
// pointers live here; keep it pure configuration data.
#define PARAMS_MAX_TITLE_CHARS 128

typedef struct Params {
    int window_width_px;
    int window_height_px;
    char window_title[PARAMS_MAX_TITLE_CHARS];
    bool vsync_on;
    float clear_color_rgba[4];
    float bee_radius_px;
    float bee_color_rgba[4];
    size_t bee_count;
    float world_width_px;
    float world_height_px;
    uint64_t rng_seed;
} Params;

void params_init_defaults(Params *params);
// Seeds Params with safe defaults (called before overrides or load pipeline).

bool params_validate(const Params *params, char *err_buf, size_t err_cap);
// Returns true when Params obey invariants; err_buf receives a short
// human-readable message on failure.

bool params_load_from_json(const char *path, Params *out_params,
                           char *err_buf, size_t err_cap);
// Placeholder for future JSON loader. Returns false while unimplemented.

#endif  // PARAMS_H
