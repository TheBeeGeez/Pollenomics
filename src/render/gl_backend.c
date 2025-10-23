#include "render.h"

#include <glad/glad.h>

#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "params.h"
#include "util/log.h"

typedef struct InstanceAttrib {
    float center[2];
    float radius;
    unsigned char color[4];
} InstanceAttrib;

#define INSTANCE_STRIDE ((GLsizei)sizeof(InstanceAttrib))

typedef struct LineVertex {
    float pos[2];
    float color[4];
} LineVertex;

#define LINE_VERTEX_STRIDE ((GLsizei)sizeof(LineVertex))

typedef struct RenderState {
    float clear_color[4];
    float default_color[4];
    unsigned char default_color_rgba[4];
    float default_radius_px;
    int fb_width;
    int fb_height;
    GLuint program;
    GLuint vao;
    GLuint quad_vbo;
    GLuint instance_vbo;
    GLint u_screen;
    GLint u_cam_center;
    GLint u_cam_zoom;
    float cam_center[2];
    float cam_zoom;
    size_t instance_capacity;
    size_t instance_buffer_size;
    unsigned char *instance_cpu_buffer;
    GLuint line_program;
    GLuint line_vao;
    GLuint line_vbo;
    GLint line_u_screen;
    GLint line_u_cam_center;
    GLint line_u_cam_zoom;
    size_t line_capacity;
    size_t line_buffer_size;
    float *line_cpu_buffer;
    GLuint hex_program;
    GLuint hex_vao;
    GLuint hex_vertex_vbo;
    GLuint hex_instance_vbo;
    GLint hex_u_screen;
    GLint hex_u_cam_center;
    GLint hex_u_cam_zoom;
    size_t hex_instance_capacity;
    size_t hex_instance_buffer_size;
    unsigned char *hex_instance_cpu_buffer;
} RenderState;

static void destroy_render_state(RenderState *state) {
    if (!state) {
        return;
    }
    if (state->program) {
        glDeleteProgram(state->program);
    }
    if (state->vao) {
        glDeleteVertexArrays(1, &state->vao);
    }
    if (state->quad_vbo) {
        glDeleteBuffers(1, &state->quad_vbo);
    }
    if (state->instance_vbo) {
        glDeleteBuffers(1, &state->instance_vbo);
    }
    if (state->line_program) {
        glDeleteProgram(state->line_program);
    }
    if (state->line_vao) {
        glDeleteVertexArrays(1, &state->line_vao);
    }
    if (state->line_vbo) {
        glDeleteBuffers(1, &state->line_vbo);
    }
    if (state->hex_program) {
        glDeleteProgram(state->hex_program);
    }
    if (state->hex_vao) {
        glDeleteVertexArrays(1, &state->hex_vao);
    }
    if (state->hex_vertex_vbo) {
        glDeleteBuffers(1, &state->hex_vertex_vbo);
    }
    if (state->hex_instance_vbo) {
        glDeleteBuffers(1, &state->hex_instance_vbo);
    }
    free(state->instance_cpu_buffer);
    free(state->line_cpu_buffer);
    free(state->hex_instance_cpu_buffer);
    free(state);
}

static GLuint compile_shader(GLenum type, const char *src, char *log_buf, size_t log_cap) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        if (log_buf && log_cap > 0) {
            glGetShaderInfoLog(shader, (GLsizei)log_cap, NULL, log_buf);
        }
        return 0;
    }
    if (log_buf && log_cap > 0) {
        log_buf[0] = '\0';
    }
    return shader;
}

static GLuint link_program(GLuint vs, GLuint fs, char *log_buf, size_t log_cap) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        if (log_buf && log_cap > 0) {
            glGetProgramInfoLog(program, (GLsizei)log_cap, NULL, log_buf);
        }
        glDeleteProgram(program);
        return 0;
    }
    if (log_buf && log_cap > 0) {
        log_buf[0] = '\0';
    }
    return program;
}

static const char *kVertexShaderSrc =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_center_world;\n"
    "layout(location=2) in float a_radius_world;\n"
    "layout(location=3) in vec4 a_color_rgba;\n"
    "uniform vec2 u_screen;\n"
    "uniform vec2 u_cam_center;\n"
    "uniform float u_cam_zoom;\n"
    "out vec2 v_px;\n"
    "out vec2 v_center_px;\n"
    "out float v_radius_px;\n"
    "out vec4 v_color_rgba;\n"
    "void main() {\n"
    "    float radius_px = a_radius_world * u_cam_zoom;\n"
    "    vec2 center_px = (a_center_world - u_cam_center) * u_cam_zoom + 0.5 * u_screen;\n"
    "    vec2 offset_px = (a_pos * 2.0 - 1.0) * radius_px;\n"
    "    vec2 px = center_px + offset_px;\n"
    "    v_px = px;\n"
    "    v_center_px = center_px;\n"
    "    v_radius_px = radius_px;\n"
    "    v_color_rgba = a_color_rgba;\n"
    "    vec2 ndc;\n"
    "    ndc.x = (px.x / u_screen.x) * 2.0 - 1.0;\n"
    "    ndc.y = 1.0 - (px.y / u_screen.y) * 2.0;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "}\n";

static const char *kFragmentShaderSrc =
    "#version 330 core\n"
    "in vec2 v_px;\n"
    "in vec2 v_center_px;\n"
    "in float v_radius_px;\n"
    "in vec4 v_color_rgba;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "    float dist = distance(v_px, v_center_px);\n"
    "    float edge = 1.5;\n"
    "    float alpha = smoothstep(v_radius_px, v_radius_px - edge, dist);\n"
    "    frag = vec4(v_color_rgba.rgb, v_color_rgba.a * alpha);\n"
    "}\n";

static const char *kHexVertexShaderSrc =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos_unit;\n"
    "layout(location=1) in vec2 a_center_world;\n"
    "layout(location=2) in float a_scale_world;\n"
    "layout(location=3) in vec4 a_color_rgba;\n"
    "uniform vec2 u_screen;\n"
    "uniform vec2 u_cam_center;\n"
    "uniform float u_cam_zoom;\n"
    "out vec4 v_color_rgba;\n"
    "void main() {\n"
    "    vec2 world_pos = a_center_world + a_pos_unit * a_scale_world;\n"
    "    vec2 px = (world_pos - u_cam_center) * u_cam_zoom + 0.5 * u_screen;\n"
    "    vec2 ndc;\n"
    "    ndc.x = (px.x / u_screen.x) * 2.0 - 1.0;\n"
    "    ndc.y = 1.0 - (px.y / u_screen.y) * 2.0;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_color_rgba = a_color_rgba;\n"
    "}\n";

static const char *kHexFragmentShaderSrc =
    "#version 330 core\n"
    "in vec4 v_color_rgba;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "    frag_color = v_color_rgba;\n"
    "}\n";

static const char *kLineVertexShaderSrc =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos_world;\n"
    "layout(location=1) in vec4 a_color_rgba;\n"
    "uniform vec2 u_screen;\n"
    "uniform vec2 u_cam_center;\n"
    "uniform float u_cam_zoom;\n"
    "out vec4 v_color_rgba;\n"
    "void main() {\n"
    "    vec2 px = (a_pos_world - u_cam_center) * u_cam_zoom + 0.5 * u_screen;\n"
    "    vec2 ndc;\n"
    "    ndc.x = (px.x / u_screen.x) * 2.0 - 1.0;\n"
    "    ndc.y = 1.0 - (px.y / u_screen.y) * 2.0;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_color_rgba = a_color_rgba;\n"
    "}\n";

static const char *kLineFragmentShaderSrc =
    "#version 330 core\n"
    "in vec4 v_color_rgba;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "    frag_color = v_color_rgba;\n"
    "}\n";

static float clamp01(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

static void configure_instance_attribs(RenderState *state) {
    glBindVertexArray(state->vao);

    glBindBuffer(GL_ARRAY_BUFFER, state->quad_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);

    glBindBuffer(GL_ARRAY_BUFFER, state->instance_vbo);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, INSTANCE_STRIDE, (void *)0);
    glVertexAttribDivisor(1, 1);

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, INSTANCE_STRIDE, (void *)(sizeof(float) * 2));
    glVertexAttribDivisor(2, 1);

    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, INSTANCE_STRIDE, (void *)(sizeof(float) * 3));
    glVertexAttribDivisor(3, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void configure_line_attribs(RenderState *state) {
    glBindVertexArray(state->line_vao);
    glBindBuffer(GL_ARRAY_BUFFER, state->line_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, LINE_VERTEX_STRIDE, (void *)offsetof(LineVertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, LINE_VERTEX_STRIDE, (void *)offsetof(LineVertex, color));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void configure_hex_attribs(RenderState *state) {
    glBindVertexArray(state->hex_vao);
    glBindBuffer(GL_ARRAY_BUFFER, state->hex_vertex_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);

    glBindBuffer(GL_ARRAY_BUFFER, state->hex_instance_vbo);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, INSTANCE_STRIDE, (void *)0);
    glVertexAttribDivisor(1, 1);

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, INSTANCE_STRIDE, (void *)(sizeof(float) * 2));
    glVertexAttribDivisor(2, 1);

    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, INSTANCE_STRIDE, (void *)(sizeof(float) * 3));
    glVertexAttribDivisor(3, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static bool ensure_line_capacity(RenderState *state, size_t desired_count) {
    if (desired_count == 0) {
        return true;
    }
    if (desired_count <= state->line_capacity) {
        return true;
    }

    size_t old_capacity = state->line_capacity;
    size_t new_capacity = old_capacity ? old_capacity : 16;
    while (new_capacity < desired_count) {
        if (new_capacity > (SIZE_MAX / 2)) {
            LOG_ERROR("render: line capacity overflow (requested %zu)", desired_count);
            return false;
        }
        new_capacity *= 2;
    }

    size_t vertex_count = new_capacity * 2;
    size_t new_bytes = vertex_count * sizeof(LineVertex);
    float *cpu_buffer = (float *)realloc(state->line_cpu_buffer, new_bytes);
    if (!cpu_buffer) {
        LOG_ERROR("render: failed to resize line CPU buffer to %zu bytes", new_bytes);
        return false;
    }
    state->line_cpu_buffer = cpu_buffer;
    state->line_capacity = new_capacity;
    state->line_buffer_size = new_bytes;

    glBindBuffer(GL_ARRAY_BUFFER, state->line_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)new_bytes, NULL, GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    configure_line_attribs(state);

    LOG_INFO("render: line buffer grow old=%zu new=%zu bytes=%zu",
             old_capacity, new_capacity, new_bytes);
    return true;
}

static void unpack_color(uint32_t packed, float out_rgba[4]) {
    out_rgba[0] = (float)((packed >> 24) & 0xFF) / 255.0f;
    out_rgba[1] = (float)((packed >> 16) & 0xFF) / 255.0f;
    out_rgba[2] = (float)((packed >> 8) & 0xFF) / 255.0f;
    out_rgba[3] = (float)(packed & 0xFF) / 255.0f;
}

static bool ensure_instance_capacity(RenderState *state, size_t desired_count) {
    if (desired_count <= state->instance_capacity) {
        return true;
    }

    size_t old_capacity = state->instance_capacity;
    size_t new_capacity = old_capacity ? old_capacity : 1024;
    while (new_capacity < desired_count) {
        if (new_capacity > (SIZE_MAX / 2)) {
            LOG_ERROR("render: instance capacity overflow (requested %zu)", desired_count);
            return false;
        }
        new_capacity *= 2;
    }

    size_t new_bytes = new_capacity * (size_t)INSTANCE_STRIDE;
    unsigned char *cpu_buffer = (unsigned char *)realloc(state->instance_cpu_buffer, new_bytes);
    if (!cpu_buffer) {
        LOG_ERROR("render: failed to resize instance CPU buffer to %zu bytes", new_bytes);
        return false;
    }
    state->instance_cpu_buffer = cpu_buffer;
    state->instance_capacity = new_capacity;
    state->instance_buffer_size = new_bytes;

    glBindBuffer(GL_ARRAY_BUFFER, state->instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)new_bytes, NULL, GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    configure_instance_attribs(state);

    LOG_INFO("render: instance buffer grow old=%zu new=%zu bytes=%zu",
             old_capacity, new_capacity, new_bytes);
    return true;
}

static void pack_instance_batch(RenderState *state,
                                size_t offset,
                                const float *positions_xy,
                                const float *radii_px,
                                const uint32_t *color_rgba,
                                size_t count) {
    if (!state->instance_cpu_buffer || count == 0) {
        return;
    }

    InstanceAttrib *attribs = (InstanceAttrib *)state->instance_cpu_buffer + offset;
    const float default_cx = state->fb_width * 0.5f;
    const float default_cy = state->fb_height * 0.5f;
    const float default_radius = state->default_radius_px > 0.0f ? state->default_radius_px : 1.0f;
    const unsigned char def_r = state->default_color_rgba[0];
    const unsigned char def_g = state->default_color_rgba[1];
    const unsigned char def_b = state->default_color_rgba[2];
    const unsigned char def_a = state->default_color_rgba[3];

    for (size_t i = 0; i < count; ++i) {
        float cx = positions_xy ? positions_xy[i * 2 + 0] : default_cx;
        float cy = positions_xy ? positions_xy[i * 2 + 1] : default_cy;
        float radius = radii_px ? radii_px[i] : default_radius;
        if (!radii_px && radius <= 0.0f) {
            radius = default_radius;
        }
        if (radius < 0.0f) {
            radius = 0.0f;
        }

        unsigned char r = def_r;
        unsigned char g = def_g;
        unsigned char b = def_b;
        unsigned char a = def_a;
        if (color_rgba) {
            uint32_t packed = color_rgba[i];
            r = (unsigned char)((packed >> 24) & 0xFF);
            g = (unsigned char)((packed >> 16) & 0xFF);
            b = (unsigned char)((packed >> 8) & 0xFF);
            a = (unsigned char)(packed & 0xFF);
        }

        attribs[i].center[0] = cx;
        attribs[i].center[1] = cy;
        attribs[i].radius = radius;
        attribs[i].color[0] = r;
        attribs[i].color[1] = g;
        attribs[i].color[2] = b;
        attribs[i].color[3] = a;
    }
}

static bool ensure_hex_instance_capacity(RenderState *state, size_t desired_count) {
    if (desired_count == 0) {
        return true;
    }
    if (desired_count <= state->hex_instance_capacity) {
        return true;
    }

    size_t old_capacity = state->hex_instance_capacity;
    size_t new_capacity = old_capacity ? old_capacity : 256;
    while (new_capacity < desired_count) {
        if (new_capacity > (SIZE_MAX / 2)) {
            LOG_ERROR("render: hex instance capacity overflow (requested %zu)", desired_count);
            return false;
        }
        new_capacity *= 2;
    }

    size_t new_bytes = new_capacity * (size_t)INSTANCE_STRIDE;
    unsigned char *cpu_buffer =
        (unsigned char *)realloc(state->hex_instance_cpu_buffer, new_bytes);
    if (!cpu_buffer) {
        LOG_ERROR("render: failed to resize hex instance CPU buffer to %zu bytes", new_bytes);
        return false;
    }
    state->hex_instance_cpu_buffer = cpu_buffer;
    state->hex_instance_capacity = new_capacity;
    state->hex_instance_buffer_size = new_bytes;

    glBindBuffer(GL_ARRAY_BUFFER, state->hex_instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)new_bytes, NULL, GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    configure_hex_attribs(state);

    LOG_INFO("render: hex instance buffer grow old=%zu new=%zu bytes=%zu",
             old_capacity,
             new_capacity,
             new_bytes);
    return true;
}

static void pack_hex_instances(RenderState *state, const RenderHexView *hex) {
    if (!state || !hex || !state->hex_instance_cpu_buffer || hex->count == 0) {
        return;
    }
    InstanceAttrib *attribs = (InstanceAttrib *)state->hex_instance_cpu_buffer;
    const float *centers = hex->centers_world_xy;
    const float *scales = hex->scale_world;
    const uint32_t *colors = hex->fill_rgba;
    const float fallback_scale = (hex->uniform_scale_world > 0.0f) ? hex->uniform_scale_world : 1.0f;
    size_t highlight_index = hex->highlight_enabled ? hex->highlight_index : (size_t)SIZE_MAX;
    uint32_t highlight_color = hex->highlight_fill_rgba;

    for (size_t i = 0; i < hex->count; ++i) {
        float cx = centers ? centers[i * 2 + 0] : 0.0f;
        float cy = centers ? centers[i * 2 + 1] : 0.0f;
        float scale = scales ? scales[i] : fallback_scale;
        if (scale <= 0.0f) {
            scale = fallback_scale;
        }

        uint32_t packed = colors ? colors[i] : 0xFFFFFFFFu;
        if (hex->highlight_enabled && i == highlight_index) {
            packed = highlight_color;
        }
        unsigned char r = (unsigned char)((packed >> 24) & 0xFFu);
        unsigned char g = (unsigned char)((packed >> 16) & 0xFFu);
        unsigned char b = (unsigned char)((packed >> 8) & 0xFFu);
        unsigned char a = (unsigned char)(packed & 0xFFu);

        attribs[i].center[0] = cx;
        attribs[i].center[1] = cy;
        attribs[i].radius = scale;
        attribs[i].color[0] = r;
        attribs[i].color[1] = g;
        attribs[i].color[2] = b;
        attribs[i].color[3] = a;
    }
}

static void render_draw_hexes(RenderState *state,
                              const RenderHexView *hex,
                              float cam_center_x,
                              float cam_center_y,
                              float cam_zoom) {
    if (!state || !hex || !hex->visible || hex->count == 0) {
        return;
    }
    if (!state->hex_program || !state->hex_vao || !state->hex_instance_vbo) {
        return;
    }
    if (!ensure_hex_instance_capacity(state, hex->count)) {
        return;
    }
    pack_hex_instances(state, hex);

    size_t byte_count = hex->count * (size_t)INSTANCE_STRIDE;
    glBindBuffer(GL_ARRAY_BUFFER, state->hex_instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)state->hex_instance_buffer_size, NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)byte_count, state->hex_instance_cpu_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    float zoom = cam_zoom > 0.0f ? cam_zoom : 1.0f;

    glUseProgram(state->hex_program);
    glUniform2f(state->hex_u_screen, (float)state->fb_width, (float)state->fb_height);
    glUniform2f(state->hex_u_cam_center, cam_center_x, cam_center_y);
    glUniform1f(state->hex_u_cam_zoom, zoom);
    glBindVertexArray(state->hex_vao);
    glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, 8, (GLsizei)hex->count);
    glBindVertexArray(0);
    glUseProgram(0);
}

bool render_init(Render *render, const Params *params) {
    if (!render || !params) {
        LOG_ERROR("render_init received null argument");
        return false;
    }
    if (render->state) {
        LOG_WARN("render_init called on non-null render state; shutting down first");
        render_shutdown(render);
    }

    if (!GLAD_GL_VERSION_3_3) {
        LOG_ERROR("render_init requires OpenGL 3.3 or newer (instancing unavailable)");
        return false;
    }

    RenderState *state = (RenderState *)calloc(1, sizeof(RenderState));
    if (!state) {
        LOG_ERROR("Failed to allocate RenderState");
        return false;
    }

    memcpy(state->clear_color, params->clear_color_rgba, sizeof(state->clear_color));
    memcpy(state->default_color, params->bee_color_rgba, sizeof(state->default_color));
    for (int i = 0; i < 4; ++i) {
        state->default_color[i] = clamp01(state->default_color[i]);
        state->default_color_rgba[i] = (unsigned char)(state->default_color[i] * 255.0f + 0.5f);
    }
    state->default_radius_px = params->bee_radius_px > 0.0f ? params->bee_radius_px : 1.0f;
    state->fb_width = params->window_width_px;
    state->fb_height = params->window_height_px;
    state->cam_center[0] = 0.0f;
    state->cam_center[1] = 0.0f;
    state->cam_zoom = 1.0f;
    state->instance_capacity = 0;
    state->instance_buffer_size = 0;
    state->instance_cpu_buffer = NULL;
    state->line_capacity = 0;
    state->line_buffer_size = 0;
    state->line_cpu_buffer = NULL;

    static const float quad_vertices[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f,
    };

    glGenVertexArrays(1, &state->vao);
    glGenBuffers(1, &state->quad_vbo);
    glGenBuffers(1, &state->instance_vbo);
    glGenVertexArrays(1, &state->hex_vao);
    glGenBuffers(1, &state->hex_vertex_vbo);
    glGenBuffers(1, &state->hex_instance_vbo);
    glGenVertexArrays(1, &state->line_vao);
    glGenBuffers(1, &state->line_vbo);

    glBindVertexArray(state->vao);
    glBindBuffer(GL_ARRAY_BUFFER, state->quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, state->instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, 0, NULL, GL_STREAM_DRAW);

    configure_instance_attribs(state);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glBindVertexArray(state->hex_vao);
    glBindBuffer(GL_ARRAY_BUFFER, state->hex_vertex_vbo);
    float hex_vertices[16];
    hex_vertices[0] = 0.0f;
    hex_vertices[1] = 0.0f;
    const float deg_to_rad = 0.01745329251994329577f;
    for (int i = 0; i < 6; ++i) {
        float angle_deg = 60.0f * (float)i - 30.0f;
        float angle_rad = angle_deg * deg_to_rad;
        hex_vertices[2 * (i + 1) + 0] = cosf(angle_rad);
        hex_vertices[2 * (i + 1) + 1] = sinf(angle_rad);
    }
    hex_vertices[14] = hex_vertices[2];
    hex_vertices[15] = hex_vertices[3];
    glBufferData(GL_ARRAY_BUFFER, sizeof(hex_vertices), hex_vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, state->hex_instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, 0, NULL, GL_STREAM_DRAW);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    configure_hex_attribs(state);

    glBindVertexArray(state->line_vao);
    glBindBuffer(GL_ARRAY_BUFFER, state->line_vbo);
    glBufferData(GL_ARRAY_BUFFER, 0, NULL, GL_STREAM_DRAW);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    configure_line_attribs(state);

    char log_buffer[2048];
    GLuint vs = compile_shader(GL_VERTEX_SHADER, kVertexShaderSrc, log_buffer, sizeof(log_buffer));
    if (!vs) {
        LOG_ERROR("Vertex shader compilation failed:\n%s", log_buffer);
        destroy_render_state(state);
        return false;
    }

    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFragmentShaderSrc, log_buffer, sizeof(log_buffer));
    if (!fs) {
        LOG_ERROR("Fragment shader compilation failed:\n%s", log_buffer);
        glDeleteShader(vs);
        destroy_render_state(state);
        return false;
    }

    state->program = link_program(vs, fs, log_buffer, sizeof(log_buffer));
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!state->program) {
        LOG_ERROR("Shader program link failed:\n%s", log_buffer);
        destroy_render_state(state);
        return false;
    }

    glUseProgram(state->program);
    state->u_screen = glGetUniformLocation(state->program, "u_screen");
    state->u_cam_center = glGetUniformLocation(state->program, "u_cam_center");
    state->u_cam_zoom = glGetUniformLocation(state->program, "u_cam_zoom");
    glUseProgram(0);

    GLuint hex_vs = compile_shader(GL_VERTEX_SHADER, kHexVertexShaderSrc, log_buffer, sizeof(log_buffer));
    if (!hex_vs) {
        LOG_ERROR("Hex vertex shader compilation failed:\n%s", log_buffer);
        destroy_render_state(state);
        return false;
    }

    GLuint hex_fs = compile_shader(GL_FRAGMENT_SHADER, kHexFragmentShaderSrc, log_buffer, sizeof(log_buffer));
    if (!hex_fs) {
        LOG_ERROR("Hex fragment shader compilation failed:\n%s", log_buffer);
        glDeleteShader(hex_vs);
        destroy_render_state(state);
        return false;
    }

    state->hex_program = link_program(hex_vs, hex_fs, log_buffer, sizeof(log_buffer));
    glDeleteShader(hex_vs);
    glDeleteShader(hex_fs);
    if (!state->hex_program) {
        LOG_ERROR("Hex shader program link failed:\n%s", log_buffer);
        destroy_render_state(state);
        return false;
    }

    glUseProgram(state->hex_program);
    state->hex_u_screen = glGetUniformLocation(state->hex_program, "u_screen");
    state->hex_u_cam_center = glGetUniformLocation(state->hex_program, "u_cam_center");
    state->hex_u_cam_zoom = glGetUniformLocation(state->hex_program, "u_cam_zoom");
    glUseProgram(0);

    GLuint line_vs = compile_shader(GL_VERTEX_SHADER, kLineVertexShaderSrc, log_buffer, sizeof(log_buffer));
    if (!line_vs) {
        LOG_ERROR("Line vertex shader compilation failed:\n%s", log_buffer);
        destroy_render_state(state);
        return false;
    }

    GLuint line_fs = compile_shader(GL_FRAGMENT_SHADER, kLineFragmentShaderSrc, log_buffer, sizeof(log_buffer));
    if (!line_fs) {
        LOG_ERROR("Line fragment shader compilation failed:\n%s", log_buffer);
        glDeleteShader(line_vs);
        destroy_render_state(state);
        return false;
    }

    state->line_program = link_program(line_vs, line_fs, log_buffer, sizeof(log_buffer));
    glDeleteShader(line_vs);
    glDeleteShader(line_fs);
    if (!state->line_program) {
        LOG_ERROR("Line shader program link failed:\n%s", log_buffer);
        destroy_render_state(state);
        return false;
    }

    glUseProgram(state->line_program);
    state->line_u_screen = glGetUniformLocation(state->line_program, "u_screen");
    state->line_u_cam_center = glGetUniformLocation(state->line_program, "u_cam_center");
    state->line_u_cam_zoom = glGetUniformLocation(state->line_program, "u_cam_zoom");
    glUseProgram(0);

    if (state->u_screen < 0 || state->u_cam_center < 0 || state->u_cam_zoom < 0) {
        LOG_WARN("render: missing camera uniforms; rendering may be incorrect");
    }
    if (state->line_u_screen < 0 || state->line_u_cam_center < 0 || state->line_u_cam_zoom < 0) {
        LOG_WARN("render: missing camera uniforms for debug lines; rendering may be incorrect");
    }

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    render->state = state;
    LOG_INFO("render: circle instancing enabled (stride=%d bytes)", INSTANCE_STRIDE);
    LOG_INFO("render: hex instancing enabled");
    return true;
}

void render_resize(Render *render, int fb_w, int fb_h) {
    if (!render || !render->state) {
        return;
    }
    RenderState *state = (RenderState *)render->state;
    if (fb_w > 0) {
        state->fb_width = fb_w;
    } else if (state->fb_width <= 0) {
        state->fb_width = 1;
    }
    if (fb_h > 0) {
        state->fb_height = fb_h;
    } else if (state->fb_height <= 0) {
        state->fb_height = 1;
    }
}

void render_set_camera(Render *render, const RenderCamera *camera) {
    if (!render || !render->state) {
        return;
    }
    RenderState *state = (RenderState *)render->state;
    if (camera) {
        state->cam_center[0] = camera->center_world[0];
        state->cam_center[1] = camera->center_world[1];
        state->cam_zoom = camera->zoom > 0.0f ? camera->zoom : 1.0f;
    } else {
        state->cam_center[0] = 0.0f;
        state->cam_center[1] = 0.0f;
        state->cam_zoom = 1.0f;
    }
}

void render_set_clear_color(Render *render, const float rgba[4]) {
    if (!render || !render->state) {
        return;
    }
    RenderState *state = (RenderState *)render->state;
    if (!rgba) {
        return;
    }
    for (int i = 0; i < 4; ++i) {
        float c = rgba[i];
        if (c < 0.0f) {
            c = 0.0f;
        }
        if (c > 1.0f) {
            c = 1.0f;
        }
        state->clear_color[i] = c;
    }
}

void render_frame(Render *render, const RenderView *view) {
    if (!render || !render->state) {
        return;
    }
    RenderState *state = (RenderState *)render->state;
    if (state->fb_width > 0 && state->fb_height > 0) {
        glViewport(0, 0, state->fb_width, state->fb_height);
    }
    glClearColor(state->clear_color[0],
                 state->clear_color[1],
                 state->clear_color[2],
                 state->clear_color[3]);
    glClear(GL_COLOR_BUFFER_BIT);

    float cam_zoom = state->cam_zoom > 0.0f ? state->cam_zoom : 1.0f;
    float cam_center_x = state->cam_center[0];
    float cam_center_y = state->cam_center[1];

    const RenderHexView *hex_view = view ? view->hex : NULL;
    bool draw_hex_before = hex_view && hex_view->visible && !hex_view->draw_on_top;
    bool draw_hex_after = hex_view && hex_view->visible && hex_view->draw_on_top;

    if (draw_hex_before) {
        render_draw_hexes(state, hex_view, cam_center_x, cam_center_y, cam_zoom);
    }

    size_t bee_count = view ? view->count : 0;
    size_t patch_count = view ? view->patch_count : 0;
    const bool patch_data_valid = view && view->patch_positions_xy && view->patch_radii_px && view->patch_fill_rgba && view->patch_ring_radii_px && view->patch_ring_rgba;
    if (!patch_data_valid) {
        patch_count = 0;
    }
    size_t total_instances = bee_count + (patch_count * 2);
    if (state->program && total_instances > 0) {
        if (ensure_instance_capacity(state, total_instances)) {
            size_t offset = 0;
            if (patch_count > 0) {
                pack_instance_batch(state,
                                    offset,
                                    view->patch_positions_xy,
                                    view->patch_radii_px,
                                    view->patch_fill_rgba,
                                    patch_count);
                offset += patch_count;
                pack_instance_batch(state,
                                    offset,
                                    view->patch_positions_xy,
                                    view->patch_ring_radii_px,
                                    view->patch_ring_rgba,
                                    patch_count);
                offset += patch_count;
            }
            pack_instance_batch(state,
                                offset,
                                view ? view->positions_xy : NULL,
                                view ? view->radii_px : NULL,
                                view ? view->color_rgba : NULL,
                                bee_count);

            size_t byte_count = total_instances * (size_t)INSTANCE_STRIDE;
            glBindBuffer(GL_ARRAY_BUFFER, state->instance_vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)state->instance_buffer_size, NULL, GL_STREAM_DRAW);
            glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)byte_count, state->instance_cpu_buffer);
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            glUseProgram(state->program);
            glUniform2f(state->u_screen, (float)state->fb_width, (float)state->fb_height);
            glUniform2f(state->u_cam_center, cam_center_x, cam_center_y);
            glUniform1f(state->u_cam_zoom, cam_zoom);
            glBindVertexArray(state->vao);
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)total_instances);
            glBindVertexArray(0);
            glUseProgram(0);
        }
    }

    if (draw_hex_after) {
        render_draw_hexes(state, hex_view, cam_center_x, cam_center_y, cam_zoom);
    }

    if (view && view->debug_line_count > 0 && view->debug_lines_xy && view->debug_line_rgba &&
        state->line_program && state->line_vao) {
        size_t line_count = view->debug_line_count;
        if (ensure_line_capacity(state, line_count) && state->line_cpu_buffer) {
            LineVertex *verts = (LineVertex *)state->line_cpu_buffer;
            const float *segments = view->debug_lines_xy;
            const uint32_t *colors = view->debug_line_rgba;
            for (size_t i = 0; i < line_count; ++i) {
                float color_rgba[4];
                unpack_color(colors[i], color_rgba);
                size_t vertex_index = i * 2;
                LineVertex *v0 = &verts[vertex_index + 0];
                LineVertex *v1 = &verts[vertex_index + 1];
                v0->pos[0] = segments[i * 4 + 0];
                v0->pos[1] = segments[i * 4 + 1];
                memcpy(v0->color, color_rgba, sizeof(color_rgba));
                v1->pos[0] = segments[i * 4 + 2];
                v1->pos[1] = segments[i * 4 + 3];
                memcpy(v1->color, color_rgba, sizeof(color_rgba));
            }

            size_t vertex_count = line_count * 2;
            size_t byte_count = vertex_count * sizeof(LineVertex);
            if (byte_count > state->line_buffer_size) {
                byte_count = state->line_buffer_size;
            }
            glBindBuffer(GL_ARRAY_BUFFER, state->line_vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)state->line_buffer_size, NULL, GL_STREAM_DRAW);
            glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)byte_count, state->line_cpu_buffer);
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            glUseProgram(state->line_program);
            glUniform2f(state->line_u_screen, (float)state->fb_width, (float)state->fb_height);
            glUniform2f(state->line_u_cam_center, cam_center_x, cam_center_y);
            glUniform1f(state->line_u_cam_zoom, cam_zoom);
            glBindVertexArray(state->line_vao);
            glLineWidth(2.0f);
            glDrawArrays(GL_LINES, 0, (GLsizei)vertex_count);
            glBindVertexArray(0);
            glUseProgram(0);
            glLineWidth(1.0f);
        }
    }
}
void render_shutdown(Render *render) {
    if (!render || !render->state) {
        return;
    }
    RenderState *state = (RenderState *)render->state;
    destroy_render_state(state);
    render->state = NULL;
}
