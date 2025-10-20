#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>

#include "params.h"

typedef struct Platform {
    void *state;  // Internal SDL/GL resources.
} Platform;

typedef struct Input {
    // Invariants: set to true when ESC pressed or window close requested.
    bool quit_requested;
    bool key_escape_down;
    bool key_space_down;
    bool key_period_down;
    bool key_escape_pressed;
    bool key_space_pressed;
    bool key_period_pressed;
} Input;

typedef struct Timing {
    // Invariants: dt_sec >= 0 (clamped to a safe range), now_sec monotonic.
    float dt_sec;
    double now_sec;
} Timing;

bool plat_init(Platform *plat, const Params *params);
// Creates window + GL context configured from Params, loads GL symbols, and sets
// the swap interval. Returns false on failure without aborting. plat must be
// zero-initialized prior to the call.

void plat_pump(Platform *plat, Input *out_input, Timing *out_timing);
// Polls pending events, populates Input snapshot (including key edges) and
// Timing delta/absolute values. Must only be called after plat_init succeeds.

void plat_swap(Platform *plat);
// Presents the current frame; legal only once per loop after plat_pump.

void plat_shutdown(Platform *plat);
// Tears down the GL context and window; idempotent.

bool plat_poll_resize(Platform *plat, int *out_fb_w, int *out_fb_h);
// Returns true when the drawable framebuffer size changed since last check.

#endif  // PLATFORM_H
