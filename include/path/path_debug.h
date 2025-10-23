#ifndef PATH_PATH_DEBUG_H
#define PATH_PATH_DEBUG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void path_debug_begin_frame(void);
bool path_debug_add_line(float x0, float y0, float x1, float y1, uint32_t color_rgba);
const float *path_debug_lines_xy(void);
const uint32_t *path_debug_lines_rgba(void);
size_t path_debug_line_count(void);

#endif  // PATH_PATH_DEBUG_H
