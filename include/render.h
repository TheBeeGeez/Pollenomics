#ifndef RENDER_H
#define RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "params.h"

typedef struct Render {
    void *state;
} Render;  // Opaque render subsystem state pointer bag.

typedef struct RenderCamera {
    float center_world[2];
    float zoom;
} RenderCamera;

typedef struct RenderHexView {
    const float *centers_world_xy;
    const float *scale_world;
    const uint32_t *fill_rgba;
    size_t count;
    float uniform_scale_world;
    bool visible;
    bool draw_on_top;
    bool highlight_enabled;
    size_t highlight_index;
    uint32_t highlight_fill_rgba;
} RenderHexView;

typedef struct RenderView {
    const float *positions_xy;     // Interleaved XY array, length = count * 2.
    const float *radii_px;         // Radius per element.
    const uint32_t *color_rgba;    // Packed 0xRRGGBBAA per element.
    size_t count;
    const float *debug_lines_xy;         // Sequence of (start,end) points, 4 floats per line.
    const uint32_t *debug_line_rgba;     // One color per line (0xRRGGBBAA).
    size_t debug_line_count;
    const RenderHexView *hex;
} RenderView;

bool render_init(Render *out, const Params *params);
// Prepares render subsystem for drawing; Params already applied to platform.

void render_resize(Render *render, int fb_w, int fb_h);
// Notifies render subsystem that the framebuffer size changed.

void render_set_camera(Render *render, const RenderCamera *camera);
// Updates the camera parameters used by subsequent render_frame calls.

void render_set_clear_color(Render *render, const float rgba[4]);
// Applies a new RGBA clear color used at the start of each frame.

void render_frame(Render *render, const RenderView *view);
// Issues draw commands for the current frame using the provided view; must not
// swap buffers.

void render_shutdown(Render *render);
// Releases GPU resources; safe to call once after render_init succeeds.

#endif  // RENDER_H

