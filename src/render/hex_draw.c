#include "hex_draw.h"

#include <glad/glad.h>

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

#ifndef HEX_DRAW_PI
#define HEX_DRAW_PI 3.14159265358979323846f
#endif

typedef struct HexInstanceAttrib {
    float center[2];
    float radius;
    unsigned char color[4];
} HexInstanceAttrib;

static const size_t kInstanceStride = sizeof(HexInstanceAttrib);

static const char *const kHexVertexShader =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_corner;\n"
    "layout(location=1) in vec2 a_center;\n"
    "layout(location=2) in float a_radius;\n"
    "layout(location=3) in vec4 a_color;\n"
    "out vec4 v_color;\n"
    "uniform vec2 u_screen;\n"
    "uniform vec2 u_cam_center;\n"
    "uniform float u_cam_zoom;\n"
    "void main(){\n"
    "    vec2 world = a_center + a_corner * a_radius;\n"
    "    vec2 px = (world - u_cam_center) * u_cam_zoom + 0.5 * u_screen;\n"
    "    vec2 ndc;\n"
    "    ndc.x = (px.x / u_screen.x) * 2.0 - 1.0;\n"
    "    ndc.y = 1.0 - (px.y / u_screen.y) * 2.0;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_color = a_color;\n"
    "}\n";

static const char *const kHexFragmentShader =
    "#version 330 core\n"
    "in vec4 v_color;\n"
    "out vec4 frag_color;\n"
    "void main(){\n"
    "    frag_color = v_color;\n"
    "}\n";

static const char *const kOutlineVertexShader =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos_world;\n"
    "uniform vec2 u_screen;\n"
    "uniform vec2 u_cam_center;\n"
    "uniform float u_cam_zoom;\n"
    "void main(){\n"
    "    vec2 px = (a_pos_world - u_cam_center) * u_cam_zoom + 0.5 * u_screen;\n"
    "    vec2 ndc;\n"
    "    ndc.x = (px.x / u_screen.x) * 2.0 - 1.0;\n"
    "    ndc.y = 1.0 - (px.y / u_screen.y) * 2.0;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "}\n";

static const char *const kOutlineFragmentShader =
    "#version 330 core\n"
    "uniform vec4 u_color;\n"
    "out vec4 frag_color;\n"
    "void main(){\n"
    "    frag_color = u_color;\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char log[1024];
        glGetShaderInfoLog(shader, (GLsizei)sizeof(log), NULL, log);
        LOG_ERROR("hex_draw: shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(GLuint vs, GLuint fs) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        char log[1024];
        glGetProgramInfoLog(program, (GLsizei)sizeof(log), NULL, log);
        LOG_ERROR("hex_draw: program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static bool ensure_instance_capacity(HexDrawState *state, size_t desired_count) {
    if (!state) {
        return false;
    }
    if (desired_count <= state->instance_capacity) {
        return true;
    }
    size_t new_capacity = state->instance_capacity ? state->instance_capacity : 256;
    while (new_capacity < desired_count) {
        if (new_capacity > (SIZE_MAX / 2)) {
            LOG_ERROR("hex_draw: instance capacity overflow (requested %zu)", desired_count);
            return false;
        }
        new_capacity *= 2;
    }
    size_t new_bytes = new_capacity * kInstanceStride;
    unsigned char *buffer = (unsigned char *)realloc(state->instance_cpu_buffer, new_bytes);
    if (!buffer) {
        LOG_ERROR("hex_draw: failed to allocate %zu bytes for instance buffer", new_bytes);
        return false;
    }
    state->instance_cpu_buffer = buffer;
    state->instance_capacity = new_capacity;
    state->instance_stride = kInstanceStride;

    glBindBuffer(GL_ARRAY_BUFFER, state->instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)new_bytes, NULL, GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return true;
}

bool hex_draw_init(HexDrawState *state) {
    if (!state) {
        return false;
    }
    memset(state, 0, sizeof(*state));

    GLuint vs = compile_shader(GL_VERTEX_SHADER, kHexVertexShader);
    if (!vs) {
        return false;
    }
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kHexFragmentShader);
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }
    state->program = link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!state->program) {
        return false;
    }

    GLuint outline_vs = compile_shader(GL_VERTEX_SHADER, kOutlineVertexShader);
    if (!outline_vs) {
        hex_draw_shutdown(state);
        return false;
    }
    GLuint outline_fs = compile_shader(GL_FRAGMENT_SHADER, kOutlineFragmentShader);
    if (!outline_fs) {
        glDeleteShader(outline_vs);
        hex_draw_shutdown(state);
        return false;
    }
    state->outline_program = link_program(outline_vs, outline_fs);
    glDeleteShader(outline_vs);
    glDeleteShader(outline_fs);
    if (!state->outline_program) {
        hex_draw_shutdown(state);
        return false;
    }

    glGenVertexArrays(1, &state->vao);
    glGenBuffers(1, &state->vertex_vbo);
    glGenBuffers(1, &state->instance_vbo);
    glGenVertexArrays(1, &state->outline_vao);
    glGenBuffers(1, &state->outline_vbo);

    static const float kAnglesDeg[] = {-30.0f, 30.0f, 90.0f, 150.0f, 210.0f, 270.0f, 330.0f};
    float corners[(sizeof(kAnglesDeg) / sizeof(kAnglesDeg[0]) + 1) * 2];
    corners[0] = 0.0f;
    corners[1] = 0.0f;
    for (size_t i = 0; i < sizeof(kAnglesDeg) / sizeof(kAnglesDeg[0]); ++i) {
        float rad = kAnglesDeg[i] * HEX_DRAW_PI / 180.0f;
        corners[(i + 1) * 2 + 0] = cosf(rad);
        corners[(i + 1) * 2 + 1] = sinf(rad);
    }

    glBindVertexArray(state->vao);
    glBindBuffer(GL_ARRAY_BUFFER, state->vertex_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);

    glBindBuffer(GL_ARRAY_BUFFER, state->instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, 0, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, (GLsizei)kInstanceStride, (void *)offsetof(HexInstanceAttrib, center));
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, (GLsizei)kInstanceStride, (void *)offsetof(HexInstanceAttrib, radius));
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)kInstanceStride, (void *)offsetof(HexInstanceAttrib, color));
    glVertexAttribDivisor(3, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glBindVertexArray(state->outline_vao);
    glBindBuffer(GL_ARRAY_BUFFER, state->outline_vbo);
    glBufferData(GL_ARRAY_BUFFER, 0, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    state->u_screen = glGetUniformLocation(state->program, "u_screen");
    state->u_cam_center = glGetUniformLocation(state->program, "u_cam_center");
    state->u_cam_zoom = glGetUniformLocation(state->program, "u_cam_zoom");
    state->outline_u_screen = glGetUniformLocation(state->outline_program, "u_screen");
    state->outline_u_cam_center = glGetUniformLocation(state->outline_program, "u_cam_center");
    state->outline_u_cam_zoom = glGetUniformLocation(state->outline_program, "u_cam_zoom");
    state->outline_u_color = glGetUniformLocation(state->outline_program, "u_color");

    state->instance_stride = kInstanceStride;
    state->outline_width_px = 3.0f;
    state->enabled = false;
    state->outline_valid = false;
    return true;
}

void hex_draw_shutdown(HexDrawState *state) {
    if (!state) {
        return;
    }
    free(state->instance_cpu_buffer);
    state->instance_cpu_buffer = NULL;
    state->instance_capacity = 0;
    state->instance_count = 0;
    if (state->program) {
        glDeleteProgram(state->program);
        state->program = 0;
    }
    if (state->outline_program) {
        glDeleteProgram(state->outline_program);
        state->outline_program = 0;
    }
    if (state->instance_vbo) {
        glDeleteBuffers(1, &state->instance_vbo);
        state->instance_vbo = 0;
    }
    if (state->vertex_vbo) {
        glDeleteBuffers(1, &state->vertex_vbo);
        state->vertex_vbo = 0;
    }
    if (state->outline_vbo) {
        glDeleteBuffers(1, &state->outline_vbo);
        state->outline_vbo = 0;
    }
    if (state->vao) {
        glDeleteVertexArrays(1, &state->vao);
        state->vao = 0;
    }
    if (state->outline_vao) {
        glDeleteVertexArrays(1, &state->outline_vao);
        state->outline_vao = 0;
    }
    memset(state->outline_vertices, 0, sizeof(state->outline_vertices));
    state->outline_valid = false;
    state->enabled = false;
}

static float clamp01(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

static void compute_outline(const HexWorld *world, const HexTile *tile, HexDrawState *state) {
    if (!world || !tile || !state) {
        return;
    }
    const float radius = world->cell_size;
    const float angles_deg[6] = {-30.0f, 30.0f, 90.0f, 150.0f, 210.0f, 270.0f};
    for (int i = 0; i < 6; ++i) {
        float rad = angles_deg[i] * HEX_DRAW_PI / 180.0f;
        state->outline_vertices[i * 2 + 0] = tile->center_x + radius * cosf(rad);
        state->outline_vertices[i * 2 + 1] = tile->center_y + radius * sinf(rad);
    }
    state->outline_valid = true;
}

bool hex_draw_update(HexDrawState *state, const HexWorld *world, const RenderHexSettings *settings) {
    if (!state) {
        return false;
    }
    state->instance_count = 0;
    state->enabled = false;
    state->outline_valid = false;

    if (!settings || !settings->enabled || !world || !world->tiles || world->cell_size <= 0.0f) {
        return true;
    }

    size_t tile_count = hex_world_tile_count(world);
    if (tile_count == 0) {
        return true;
    }
    if (!ensure_instance_capacity(state, tile_count)) {
        return false;
    }

    const RenderHexPalette *palette = &settings->palette;
    state->outline_width_px = palette->outline_width_px > 0.0f ? palette->outline_width_px : 3.0f;
    memcpy(state->outline_color, palette->outline_rgba, sizeof(state->outline_color));

    HexInstanceAttrib *attribs = (HexInstanceAttrib *)state->instance_cpu_buffer;
    size_t instance_index = 0;
    const int selected_index = settings->selected_index;

    for (size_t i = 0; i < tile_count; ++i) {
        const HexTile *tile = &world->tiles[i];
        if (!(tile->flags & HEX_TILE_FLAG_VISIBLE)) {
            continue;
        }
        if (tile->terrain >= HEX_TERRAIN_COUNT) {
            continue;
        }
        HexInstanceAttrib *inst = &attribs[instance_index++];
        inst->center[0] = tile->center_x;
        inst->center[1] = tile->center_y;
        inst->radius = world->cell_size;

        float rgba[4];
        memcpy(rgba, palette->terrain_rgba[tile->terrain], sizeof(rgba));
        if ((int)i == selected_index) {
            rgba[0] = clamp01(rgba[0] * palette->selected_multiplier);
            rgba[1] = clamp01(rgba[1] * palette->selected_multiplier);
            rgba[2] = clamp01(rgba[2] * palette->selected_multiplier);
            rgba[3] = palette->selected_alpha;
            compute_outline(world, tile, state);
        }
        for (int c = 0; c < 4; ++c) {
            float channel = clamp01(rgba[c]);
            inst->color[c] = (unsigned char)(channel * 255.0f + 0.5f);
        }
    }

    state->instance_count = instance_index;
    state->enabled = instance_index > 0;
    if (!state->outline_valid) {
        memset(state->outline_vertices, 0, sizeof(state->outline_vertices));
    }
    return true;
}

void hex_draw_draw(HexDrawState *state,
                   const RenderHexSettings *settings,
                   const float cam_center[2],
                   float cam_zoom,
                   int fb_width,
                   int fb_height) {
    if (!state || !settings || !state->enabled || state->instance_count == 0) {
        return;
    }
    if (!state->program || !state->vao) {
        return;
    }

    if (cam_zoom <= 0.0f) {
        cam_zoom = 1.0f;
    }

    size_t byte_count = state->instance_count * state->instance_stride;
    glBindBuffer(GL_ARRAY_BUFFER, state->instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(state->instance_capacity * state->instance_stride), NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)byte_count, state->instance_cpu_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glUseProgram(state->program);
    glUniform2f(state->u_screen, (float)fb_width, (float)fb_height);
    glUniform2f(state->u_cam_center, cam_center ? cam_center[0] : 0.0f, cam_center ? cam_center[1] : 0.0f);
    glUniform1f(state->u_cam_zoom, cam_zoom);
    glBindVertexArray(state->vao);
    glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, 7, (GLsizei)state->instance_count);
    glBindVertexArray(0);
    glUseProgram(0);

    if (state->outline_valid && state->outline_program && state->outline_vao) {
        glBindBuffer(GL_ARRAY_BUFFER, state->outline_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(state->outline_vertices), state->outline_vertices, GL_STREAM_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glUseProgram(state->outline_program);
        glUniform2f(state->outline_u_screen, (float)fb_width, (float)fb_height);
        glUniform2f(state->outline_u_cam_center, cam_center ? cam_center[0] : 0.0f, cam_center ? cam_center[1] : 0.0f);
        glUniform1f(state->outline_u_cam_zoom, cam_zoom);
        glUniform4f(state->outline_u_color,
                    clamp01(state->outline_color[0]),
                    clamp01(state->outline_color[1]),
                    clamp01(state->outline_color[2]),
                    clamp01(state->outline_color[3]));

        glBindVertexArray(state->outline_vao);
        glBindBuffer(GL_ARRAY_BUFFER, state->outline_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(state->outline_vertices), state->outline_vertices, GL_STREAM_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glLineWidth(state->outline_width_px > 0.0f ? state->outline_width_px : 3.0f);
        glDrawArrays(GL_LINE_LOOP, 0, 6);
        glLineWidth(1.0f);
        glBindVertexArray(0);
        glUseProgram(0);
    }
}
