#ifndef HEX_DRAW_H
#define HEX_DRAW_H

#include <stdbool.h>
#include <stddef.h>

#include "hex.h"
#include "render_hex.h"

typedef struct HexDrawState {
    unsigned char *instance_cpu_buffer;
    size_t instance_capacity;
    size_t instance_count;
    size_t instance_stride;
    float outline_vertices[12];
    float outline_color[4];
    float outline_width_px;
    bool outline_valid;
    bool enabled;

    unsigned int vao;
    unsigned int vertex_vbo;
    unsigned int instance_vbo;
    unsigned int program;
    int u_screen;
    int u_cam_center;
    int u_cam_zoom;

    unsigned int outline_vao;
    unsigned int outline_vbo;
    unsigned int outline_program;
    int outline_u_screen;
    int outline_u_cam_center;
    int outline_u_cam_zoom;
    int outline_u_color;
} HexDrawState;

bool hex_draw_init(HexDrawState *state);
void hex_draw_shutdown(HexDrawState *state);
bool hex_draw_update(HexDrawState *state, const HexWorld *world, const RenderHexSettings *settings);
void hex_draw_draw(HexDrawState *state,
                   const RenderHexSettings *settings,
                   const float cam_center[2],
                   float cam_zoom,
                   int fb_width,
                   int fb_height);

#endif  // HEX_DRAW_H
