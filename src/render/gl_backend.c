#include "render.h"

#include <glad/glad.h>

#include <stdint.h>
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
    free(state->instance_cpu_buffer);
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

static void pack_instance_data(RenderState *state, const RenderView *view, size_t count) {
    if (!state->instance_cpu_buffer || !view) {
        return;
    }

    InstanceAttrib *attribs = (InstanceAttrib *)state->instance_cpu_buffer;
    const float *positions = view->positions_xy;
    const float *radii = view->radii_px;
    const uint32_t *colors = view->color_rgba;
    const float default_cx = state->fb_width * 0.5f;
    const float default_cy = state->fb_height * 0.5f;
    const float default_radius = state->default_radius_px > 0.0f ? state->default_radius_px : 1.0f;
    const unsigned char def_r = state->default_color_rgba[0];
    const unsigned char def_g = state->default_color_rgba[1];
    const unsigned char def_b = state->default_color_rgba[2];
    const unsigned char def_a = state->default_color_rgba[3];

    for (size_t i = 0; i < count; ++i) {
        float cx = positions ? positions[i * 2 + 0] : default_cx;
        float cy = positions ? positions[i * 2 + 1] : default_cy;
        float radius = radii ? radii[i] : default_radius;
        if (radius <= 0.0f) {
            radius = default_radius;
        }

        unsigned char r = def_r;
        unsigned char g = def_g;
        unsigned char b = def_b;
        unsigned char a = def_a;
        if (colors) {
            uint32_t packed = colors[i];
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

    static const float quad_vertices[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f,
    };

    glGenVertexArrays(1, &state->vao);
    glGenBuffers(1, &state->quad_vbo);
    glGenBuffers(1, &state->instance_vbo);

    glBindVertexArray(state->vao);
    glBindBuffer(GL_ARRAY_BUFFER, state->quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, state->instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, 0, NULL, GL_STREAM_DRAW);

    configure_instance_attribs(state);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

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

    if (state->u_screen < 0 || state->u_cam_center < 0 || state->u_cam_zoom < 0) {
        LOG_WARN("render: missing camera uniforms; rendering may be incorrect");
    }

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    render->state = state;
    LOG_INFO("render: circle instancing enabled (stride=%d bytes)", INSTANCE_STRIDE);
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

    size_t instance_count = view ? view->count : 0;
    if (!state->program || instance_count == 0) {
        return;
    }

    if (!ensure_instance_capacity(state, instance_count)) {
        return;
    }

    pack_instance_data(state, view, instance_count);

    float cam_zoom = state->cam_zoom;
    if (cam_zoom <= 0.0f) {
        cam_zoom = 1.0f;
    }
    float cam_center_x = state->cam_center[0];
    float cam_center_y = state->cam_center[1];

    size_t byte_count = instance_count * (size_t)INSTANCE_STRIDE;
    glBindBuffer(GL_ARRAY_BUFFER, state->instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)state->instance_buffer_size, NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)byte_count, state->instance_cpu_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glUseProgram(state->program);
    glUniform2f(state->u_screen, (float)state->fb_width, (float)state->fb_height);
    glUniform2f(state->u_cam_center, cam_center_x, cam_center_y);
    glUniform1f(state->u_cam_zoom, cam_zoom);
    glBindVertexArray(state->vao);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)instance_count);
    glBindVertexArray(0);
    glUseProgram(0);
}
void render_shutdown(Render *render) {
    if (!render || !render->state) {
        return;
    }
    RenderState *state = (RenderState *)render->state;
    destroy_render_state(state);
    render->state = NULL;
}
