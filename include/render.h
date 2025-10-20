#ifndef RENDER_H
#define RENDER_H

#include <stdbool.h>

#include "params.h"

typedef struct Render Render;  // Opaque render subsystem state.

bool render_init(Render *out, const Params *params);
// Prepares render subsystem for drawing; Params already applied to platform.

void render_resize(Render *render, int fb_w, int fb_h);
// Notifies render subsystem that the framebuffer size changed.

void render_frame(Render *render);
// Issues draw commands for the current frame; must not swap buffers.

void render_shutdown(Render *render);
// Releases GPU resources; safe to call once after render_init succeeds.

#endif  // RENDER_H
