#ifndef RENDER_HEX_H
#define RENDER_HEX_H

#include <stdbool.h>
#include <stddef.h>

#include "hex.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RenderHexPalette {
    float terrain_rgba[HEX_TERRAIN_COUNT][4];
    float selected_multiplier;
    float selected_alpha;
    float outline_rgba[4];
    float outline_width_px;
} RenderHexPalette;

typedef struct RenderHexSettings {
    bool enabled;
    bool draw_on_top;
    int selected_index;
    RenderHexPalette palette;
} RenderHexSettings;

#ifdef __cplusplus
}
#endif

#endif  // RENDER_HEX_H
