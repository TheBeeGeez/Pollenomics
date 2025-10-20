#include "render.h"

#include <glad/glad.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "params.h"
#include "util/log.h"

typedef struct RenderState {
    float clear_color[4];
    float bee_color[4];
    float bee_radius_px;
    int fb_width;
    int fb_height;
    GLuint program;
    GLuint vao;
    GLuint vbo;
    GLint u_screen;
    GLint u_pos;
    GLint u_size;
    GLint u_center;
    GLint u_radius;
    GLint u_color;
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
    if (state->vbo) {
        glDeleteBuffers(1, &state->vbo);
    }
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
    "uniform vec2 u_screen;\n"
    "uniform vec2 u_pos;\n"
    "uniform vec2 u_size;\n"
    "out vec2 v_px;\n"
    "void main() {\n"
    "    vec2 px = u_pos + a_pos * u_size;\n"
    "    v_px = px;\n"
    "    vec2 ndc;\n"
    "    ndc.x = (px.x / u_screen.x) * 2.0 - 1.0;\n"
    "    ndc.y = 1.0 - (px.y / u_screen.y) * 2.0;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "}\n";

static const char *kFragmentShaderSrc =
    "#version 330 core\n"
    "in vec2 v_px;\n"
    "out vec4 frag;\n"
    "uniform vec2 u_center;\n"
    "uniform float u_radius;\n"
    "uniform vec4 u_color;\n"
    "void main() {\n"
    "    float d = distance(v_px, u_center);\n"
    "    float alpha = smoothstep(u_radius, u_radius - 1.5, d);\n"
    "    frag = vec4(u_color.rgb, u_color.a * alpha);\n"
    "}\n";

bool render_init(Render *render, const Params *params) {
    if (!render || !params) {
        LOG_ERROR("render_init received null argument");
        return false;
    }
    if (render->state) {
        LOG_WARN("render_init called on non-null render state; shutting down first");
        render_shutdown(render);
    }

    RenderState *state = (RenderState *)calloc(1, sizeof(RenderState));
    if (!state) {
        LOG_ERROR("Failed to allocate RenderState");
        return false;
    }

    memcpy(state->clear_color, params->clear_color_rgba, sizeof(state->clear_color));
    memcpy(state->bee_color, params->bee_color_rgba, sizeof(state->bee_color));
    state->bee_radius_px = params->bee_radius_px;
    state->fb_width = params->window_width_px;
    state->fb_height = params->window_height_px;

    static const float quad_vertices[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f,
    };

    glGenVertexArrays(1, &state->vao);
    glGenBuffers(1, &state->vbo);
    glBindVertexArray(state->vao);
    glBindBuffer(GL_ARRAY_BUFFER, state->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
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
    state->u_pos = glGetUniformLocation(state->program, "u_pos");
    state->u_size = glGetUniformLocation(state->program, "u_size");
    state->u_center = glGetUniformLocation(state->program, "u_center");
    state->u_radius = glGetUniformLocation(state->program, "u_radius");
    state->u_color = glGetUniformLocation(state->program, "u_color");
    glUseProgram(0);

    if (state->u_screen < 0 || state->u_pos < 0 || state->u_size < 0 ||
        state->u_center < 0 || state->u_radius < 0 || state->u_color < 0) {
        LOG_WARN("Render uniforms missing; program may not render correctly");
    }

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    render->state = state;
    return true;
}

void render_resize(Render *render, int fb_w, int fb_h) {
    if (!render || !render->state) {
        return;
    }
    RenderState *state = (RenderState *)render->state;
    state->fb_width = fb_w;
    state->fb_height = fb_h;
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

    if (!state->program || !view || view->count == 0) {
        return;
    }

    const float *positions = view->positions_xy;
    const float *radii = view->radii_px;
    const uint32_t *colors = view->color_rgba;

    glUseProgram(state->program);
    glUniform2f(state->u_screen, (float)state->fb_width, (float)state->fb_height);
    glBindVertexArray(state->vao);

    for (size_t i = 0; i < view->count; ++i) {
        float cx = positions ? positions[i * 2 + 0] : (float)state->fb_width * 0.5f;
        float cy = positions ? positions[i * 2 + 1] : (float)state->fb_height * 0.5f;
        float radius = radii ? radii[i] : state->bee_radius_px;
        if (radius <= 0.0f) {
            radius = state->bee_radius_px;
        }
        float color_r = state->bee_color[0];
        float color_g = state->bee_color[1];
        float color_b = state->bee_color[2];
        float color_a = state->bee_color[3];
        if (colors) {
            uint32_t packed = colors[i];
            color_r = ((float)((packed >> 24) & 0xFF)) / 255.0f;
            color_g = ((float)((packed >> 16) & 0xFF)) / 255.0f;
            color_b = ((float)((packed >> 8) & 0xFF)) / 255.0f;
            color_a = ((float)(packed & 0xFF)) / 255.0f;
        }
        float pos_x = cx - radius;
        float pos_y = cy - radius;
        float size_x = radius * 2.0f;
        float size_y = radius * 2.0f;
        glUniform2f(state->u_pos, pos_x, pos_y);
        glUniform2f(state->u_size, size_x, size_y);
        glUniform2f(state->u_center, cx, cy);
        glUniform1f(state->u_radius, radius);
        glUniform4f(state->u_color, color_r, color_g, color_b, color_a);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

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
