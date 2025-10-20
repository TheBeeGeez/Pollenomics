#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>

#include "params.h"

typedef struct Platform Platform;  // Opaque platform state (window, GL context).

typedef struct Input {
    // Invariants: set to true when ESC pressed or window close requested.
    bool quit_requested;
    bool key_escape_down;
} Input;

typedef struct Timing {
    // Invariants: dt_sec >= 0, now_sec monotonically increases between frames.
    float dt_sec;
    double now_sec;
} Timing;

bool plat_init(Platform *out, const Params *params);
// Creates window + GL context configured from Params, loads GL symbols, and
// sets the swap interval. Returns false on failure without aborting.

void plat_pump(Platform *plat, Input *out_input, Timing *out_timing);
// Polls pending events, populates Input snapshot and Timing delta/absolute
// values. Must only be called after plat_init succeeds.

void plat_swap(Platform *plat);
// Presents the current frame; legal only once per loop after plat_pump.

void plat_shutdown(Platform *plat);
// Tears down the GL context and window; idempotent.

#endif  // PLATFORM_H
