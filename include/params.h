#ifndef PARAMS_H
#define PARAMS_H

#include <stdbool.h>

// Params holds immutable configuration values supplied at boot.
// Invariants: window dimensions > 0 and title non-empty at use sites.
// No runtime state or pointers live here; keep it pure configuration data.
#define PARAMS_MAX_TITLE_CHARS 128

typedef struct Params {
    int window_width_px;
    int window_height_px;
    char window_title[PARAMS_MAX_TITLE_CHARS];
    bool vsync_on;
} Params;

#endif  // PARAMS_H
