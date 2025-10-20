#include "ui.h"

#include <glad/glad.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

#define UI_PANEL_WIDTH 320.0f
#define UI_PANEL_MARGIN 16.0f
#define UI_HAMBURGER_SIZE 28.0f
#define UI_SLIDER_HEIGHT 18.0f
#define UI_SLIDER_SPACING 40.0f
#define UI_FONT_SCALE 2.0f
#define UI_CHAR_WIDTH (5.0f * UI_FONT_SCALE)
#define UI_CHAR_HEIGHT (7.0f * UI_FONT_SCALE)
#define UI_CHAR_ADVANCE (UI_CHAR_WIDTH + UI_FONT_SCALE)

typedef struct {
    float x, y, w, h;
} UiRect;

static float ui_clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

typedef struct {
    float r, g, b, a;
} UiColor;

typedef struct {
    float x, y;
    float r, g, b, a;
} UiVertex;

typedef struct {
    char ch;
    unsigned char rows[7];
} UiGlyph;

typedef struct {
    bool panel_open;
    bool mouse_over_panel;
    bool capturing_mouse;
    bool prev_mouse_down;
    int active_slider;
    bool dirty;
    bool reinit_required;
    bool has_params;
    bool sim_paused;
    float mouse_x;
    float mouse_y;
    Params baseline;
    Params *runtime;

    UiVertex *vertices;
    size_t vert_count;
    size_t vert_capacity;

    bool wants_mouse;
    bool wants_keyboard;

    bool action_toggle_pause;
    bool action_step;
    bool action_apply;
    bool action_reset;
    bool action_reinit;

    GLuint program;
    GLuint vao;
    GLuint vbo;
    GLint resolution_uniform;
} UiState;

static UiState g_ui;

static const char *const UI_VERTEX_SHADER =
    "#version 330 core\n"
    "layout(location = 0) in vec2 a_pos;\n"
    "layout(location = 1) in vec4 a_color;\n"
    "out vec4 v_color;\n"
    "uniform vec2 u_resolution;\n"
    "void main(){\n"
    "    vec2 ndc = vec2((a_pos.x / u_resolution.x)*2.0 - 1.0, 1.0 - (a_pos.y / u_resolution.y)*2.0);\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_color = a_color;\n"
    "}\n";

static const char *const UI_FRAGMENT_SHADER =
    "#version 330 core\n"
    "in vec4 v_color;\n"
    "out vec4 frag_color;\n"
    "void main(){\n"
    "    frag_color = v_color;\n"
    "}\n";

static UiColor ui_color_rgba(float r, float g, float b, float a) {
    UiColor c = {r, g, b, a};
    return c;
}

static bool ui_rect_contains(const UiRect *rect, float px, float py) {
    return px >= rect->x && px <= rect->x + rect->w && py >= rect->y && py <= rect->y + rect->h;
}

static void ui_reserve_vertices(size_t additional) {
    if (g_ui.vert_count + additional <= g_ui.vert_capacity) {
        return;
    }
    size_t new_capacity = g_ui.vert_capacity ? g_ui.vert_capacity : 1024;
    while (g_ui.vert_count + additional > new_capacity) {
        new_capacity *= 2;
    }
    UiVertex *new_vertices = (UiVertex *)realloc(g_ui.vertices, new_capacity * sizeof(UiVertex));
    if (!new_vertices) {
        LOG_ERROR("ui: failed to grow vertex buffer");
        return;
    }
    g_ui.vertices = new_vertices;
    g_ui.vert_capacity = new_capacity;
}

static void ui_push_vertex(float x, float y, UiColor color) {
    ui_reserve_vertices(1);
    if (!g_ui.vertices) {
        return;
    }
    UiVertex *v = &g_ui.vertices[g_ui.vert_count++];
    v->x = x;
    v->y = y;
    v->r = color.r;
    v->g = color.g;
    v->b = color.b;
    v->a = color.a;
}

static size_t ui_add_rect(float x, float y, float w, float h, UiColor color) {
    ui_reserve_vertices(6);
    if (!g_ui.vertices) {
        return g_ui.vert_count;
    }
    size_t start = g_ui.vert_count;
    ui_push_vertex(x, y, color);
    ui_push_vertex(x + w, y, color);
    ui_push_vertex(x + w, y + h, color);

    ui_push_vertex(x, y, color);
    ui_push_vertex(x + w, y + h, color);
    ui_push_vertex(x, y + h, color);
    return start;
}

static void ui_update_rect(size_t start, float x, float y, float w, float h) {
    if (!g_ui.vertices || start + 6 > g_ui.vert_count) {
        return;
    }
    UiVertex *verts = &g_ui.vertices[start];
    verts[0].x = x;
    verts[0].y = y;
    verts[1].x = x + w;
    verts[1].y = y;
    verts[2].x = x + w;
    verts[2].y = y + h;

    verts[3].x = x;
    verts[3].y = y;
    verts[4].x = x + w;
    verts[4].y = y + h;
    verts[5].x = x;
    verts[5].y = y + h;
}

static float ui_measure_text(const char *text) {
    if (!text) {
        return 0.0f;
    }
    float line_width = 0.0f;
    float max_width = 0.0f;
    while (*text) {
        if (*text == '\n') {
            if (line_width > max_width) {
                max_width = line_width;
            }
            line_width = 0.0f;
        } else {
            line_width += UI_CHAR_ADVANCE;
        }
        ++text;
    }
    if (line_width > max_width) {
        max_width = line_width;
    }
    return max_width;
}

#define GLYPH(ch, r0, r1, r2, r3, r4, r5, r6) \
    { ch, { r0, r1, r2, r3, r4, r5, r6 } }

typedef struct {
    char ch;
    const char *rows[7];
} UiGlyphPattern;

static unsigned char ui_row_bits_from_pattern(const char *pattern) {
    unsigned char result = 0;
    for (int i = 0; i < 5; ++i) {
        result <<= 1;
        if (pattern[i] == '#') {
            result |= 1;
        }
    }
    return result;
}

static const UiGlyphPattern g_glyph_patterns[] = {
    GLYPH(' ', ".....", ".....", ".....", ".....", ".....", ".....", "....."),
    GLYPH('0', " ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "),
    GLYPH('1', "  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "),
    GLYPH('2', " ### ", "#   #", "    #", "  ## ", " #   ", "#    ", "#####"),
    GLYPH('3', " ### ", "#   #", "    #", " ### ", "    #", "#   #", " ### "),
    GLYPH('4', "   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "),
    GLYPH('5', "#####", "#    ", "#    ", "#### ", "    #", "#   #", " ### "),
    GLYPH('6', " ### ", "#   #", "#    ", "#### ", "#   #", "#   #", " ### "),
    GLYPH('7', "#####", "    #", "   # ", "  #  ", "  #  ", "  #  ", "  #  "),
    GLYPH('8', " ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "),
    GLYPH('9', " ### ", "#   #", "#   #", " ####", "    #", "#   #", " ### "),
    GLYPH('A', " ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"),
    GLYPH('B', "#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "),
    GLYPH('C', " ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "),
    GLYPH('D', "#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "),
    GLYPH('E', "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"),
    GLYPH('F', "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "),
    GLYPH('G', " ### ", "#   #", "#    ", "#  ##", "#   #", "#   #", " ### "),
    GLYPH('H', "#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"),
    GLYPH('I', " ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "),
    GLYPH('J', "  ###", "   # ", "   # ", "   # ", "#  # ", "#  # ", " ##  "),
    GLYPH('K', "#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"),
    GLYPH('L', "#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"),
    GLYPH('M', "#   #", "## ##", "# # #", "#   #", "#   #", "#   #", "#   #"),
    GLYPH('N', "#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"),
    GLYPH('O', " ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "),
    GLYPH('P', "#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "),
    GLYPH('Q', " ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"),
    GLYPH('R', "#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"),
    GLYPH('S', " ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "),
    GLYPH('T', "#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "),
    GLYPH('U', "#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "),
    GLYPH('V', "#   #", "#   #", "#   #", "#   #", " # # ", " # # ", "  #  "),
    GLYPH('W', "#   #", "#   #", "# # #", "# # #", "# # #", "## ##", "#   #"),
    GLYPH('X', "#   #", " # # ", "  #  ", "  #  ", "  #  ", " # # ", "#   #"),
    GLYPH('Y', "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "),
    GLYPH('Z', "#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"),
    GLYPH(':', ".....", "  #  ", ".....", ".....", "  #  ", ".....", "....."),
    GLYPH('.', ".....", ".....", ".....", ".....", ".....", "  #  ", "....."),
    GLYPH('-', ".....", ".....", ".....", " ### ", ".....", ".....", "....."),
    GLYPH('+', ".....", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "....."),
    GLYPH('(', "   # ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "   # "),
    GLYPH(')', " #   ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " #   "),
    GLYPH('/', "    #", "   # ", "   # ", "  #  ", " #   ", " #   ", "#    "),
    GLYPH('%', "#   #", "   # ", "  #  ", "  #  ", " #   ", " #   ", "#   #")
};

static UiGlyph g_glyphs[sizeof(g_glyph_patterns) / sizeof(g_glyph_patterns[0])];
static size_t g_glyph_count = 0;
static bool g_glyphs_ready = false;

static void ui_build_glyph_cache(void) {
    if (g_glyphs_ready) {
        return;
    }
    g_glyph_count = sizeof(g_glyph_patterns) / sizeof(g_glyph_patterns[0]);
    for (size_t i = 0; i < g_glyph_count; ++i) {
        g_glyphs[i].ch = g_glyph_patterns[i].ch;
        for (int row = 0; row < 7; ++row) {
            g_glyphs[i].rows[row] = ui_row_bits_from_pattern(g_glyph_patterns[i].rows[row]);
        }
    }
    g_glyphs_ready = true;
}

static const UiGlyph *ui_find_glyph(char ch) {
    if (!g_glyphs_ready) {
        ui_build_glyph_cache();
    }
    for (size_t i = 0; i < g_glyph_count; ++i) {
        if (g_glyphs[i].ch == ch) {
            return &g_glyphs[i];
        }
    }
    return &g_glyphs[0];
}

static void ui_draw_text(float x, float y, const char *text, UiColor color) {
    float cursor_x = x;
    float cursor_y = y;
    while (*text) {
        char ch = *text++;
        if (ch == '\n') {
            cursor_x = x;
            cursor_y += UI_CHAR_HEIGHT + UI_FONT_SCALE;
            continue;
        }
        const UiGlyph *glyph = ui_find_glyph(ch);
        for (int row = 0; row < 7; ++row) {
            unsigned char bits = glyph->rows[row];
            for (int col = 0; col < 5; ++col) {
                if (bits & (1 << (4 - col))) {
                    float px = cursor_x + col * UI_FONT_SCALE;
                    float py = cursor_y + row * UI_FONT_SCALE;
                    ui_add_rect(px, py, UI_FONT_SCALE, UI_FONT_SCALE, color);
                }
            }
        }
        cursor_x += UI_CHAR_ADVANCE;
    }
}

static GLuint ui_create_shader(const char *vs_src, const char *fs_src) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    GLint compiled = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];
        glGetShaderInfoLog(vs, (GLsizei)sizeof(log), NULL, log);
        LOG_ERROR("ui: vertex shader compile error: %s", log);
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];
        glGetShaderInfoLog(fs, (GLsizei)sizeof(log), NULL, log);
        LOG_ERROR("ui: fragment shader compile error: %s", log);
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(program, (GLsizei)sizeof(log), NULL, log);
        LOG_ERROR("ui: shader link error: %s", log);
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

void ui_init(void) {
    memset(&g_ui, 0, sizeof(g_ui));
    ui_build_glyph_cache();
    g_ui.program = ui_create_shader(UI_VERTEX_SHADER, UI_FRAGMENT_SHADER);
    g_ui.resolution_uniform = glGetUniformLocation(g_ui.program, "u_resolution");

    glGenVertexArrays(1, &g_ui.vao);
    glGenBuffers(1, &g_ui.vbo);
    glBindVertexArray(g_ui.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_ui.vbo);
    glBufferData(GL_ARRAY_BUFFER, 0, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UiVertex), (void *)offsetof(UiVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(UiVertex), (void *)offsetof(UiVertex, r));
    glBindVertexArray(0);

    g_ui.vert_capacity = 2048;
    g_ui.vertices = (UiVertex *)malloc(g_ui.vert_capacity * sizeof(UiVertex));
    g_ui.active_slider = -1;
}

void ui_shutdown(void) {
    free(g_ui.vertices);
    g_ui.vertices = NULL;
    g_ui.vert_capacity = 0;
    g_ui.vert_count = 0;
    g_glyphs_ready = false;
    g_glyph_count = 0;

    if (g_ui.vbo) {
        glDeleteBuffers(1, &g_ui.vbo);
        g_ui.vbo = 0;
    }
    if (g_ui.vao) {
        glDeleteVertexArrays(1, &g_ui.vao);
        g_ui.vao = 0;
    }
    if (g_ui.program) {
        glDeleteProgram(g_ui.program);
        g_ui.program = 0;
    }
}

void ui_sync_to_params(const Params *baseline, Params *runtime) {
    if (!baseline || !runtime) {
        g_ui.has_params = false;
        g_ui.runtime = NULL;
        return;
    }
    g_ui.baseline = *baseline;
    g_ui.runtime = runtime;
    g_ui.has_params = true;
    g_ui.dirty = false;
    g_ui.reinit_required = false;
}

static void ui_begin_frame(const Input *input) {
    g_ui.vert_count = 0;
    g_ui.action_toggle_pause = false;
    g_ui.action_step = false;
    g_ui.action_apply = false;
    g_ui.action_reset = false;
    g_ui.action_reinit = false;
    g_ui.wants_mouse = false;
    g_ui.wants_keyboard = false;

    if (!g_ui.has_params || !g_ui.runtime) {
        return;
    }

    g_ui.mouse_x = input ? input->mouse_x_px : 0.0f;
    g_ui.mouse_y = input ? input->mouse_y_px : 0.0f;
    bool mouse_down = input ? input->mouse_left_down : false;
    bool mouse_pressed = input ? input->mouse_left_pressed : false;

    UiColor panel_bg = ui_color_rgba(0.08f, 0.08f, 0.10f, 0.92f);
    UiColor accent = ui_color_rgba(0.25f, 0.60f, 0.98f, 1.0f);
    UiColor border = ui_color_rgba(0.2f, 0.2f, 0.2f, 1.0f);
    UiColor text = ui_color_rgba(1.0f, 1.0f, 1.0f, 1.0f);

    UiRect hamburger = {UI_PANEL_MARGIN, UI_PANEL_MARGIN, UI_HAMBURGER_SIZE, UI_HAMBURGER_SIZE};
    bool hamburger_hover = ui_rect_contains(&hamburger, g_ui.mouse_x, g_ui.mouse_y);
    UiColor burger_col = hamburger_hover ? accent : ui_color_rgba(0.9f, 0.9f, 0.9f, 1.0f);
    ui_add_rect(hamburger.x, hamburger.y, hamburger.w, hamburger.h, ui_color_rgba(0.15f, 0.15f, 0.18f, 0.95f));
    float line_padding = 6.0f;
    for (int i = 0; i < 3; ++i) {
        float ly = hamburger.y + 6.0f + i * (line_padding + 4.0f);
        ui_add_rect(hamburger.x + 6.0f, ly, hamburger.w - 12.0f, 4.0f, burger_col);
    }
    if (mouse_pressed && hamburger_hover) {
        g_ui.panel_open = !g_ui.panel_open;
    }

    UiRect panel_rect = {UI_PANEL_MARGIN, UI_PANEL_MARGIN + UI_HAMBURGER_SIZE + 12.0f, UI_PANEL_WIDTH, 0.0f};

    if (!g_ui.panel_open) {
        g_ui.mouse_over_panel = false;
        g_ui.wants_mouse = g_ui.capturing_mouse;
        g_ui.wants_keyboard = false;
        g_ui.prev_mouse_down = mouse_down;
        return;
    }

    float cursor_y = panel_rect.y + 18.0f;
    float content_width = UI_PANEL_WIDTH - 40.0f;
    float panel_max_x = panel_rect.x + UI_PANEL_WIDTH;

    size_t panel_bg_start = ui_add_rect(panel_rect.x, panel_rect.y, UI_PANEL_WIDTH, 520.0f, panel_bg);
    size_t panel_border_start = ui_add_rect(panel_rect.x, panel_rect.y, UI_PANEL_WIDTH, 520.0f, border);

    float text_x = panel_rect.x + 20.0f;
    ui_draw_text(text_x, cursor_y, "SIM CONTROLS", text);
    panel_max_x = fmaxf(panel_max_x, text_x + ui_measure_text("SIM CONTROLS"));
    cursor_y += 24.0f;

    typedef struct {
        const char *label;
        float min_value;
        float max_value;
        float step;
        float *value;
        int id;
    } SliderSpec;

    SliderSpec sliders[] = {
        {"MIN SPEED", 0.0f, 200.0f, 1.0f, &g_ui.runtime->motion_min_speed, 0},
        {"MAX SPEED", 0.0f, 200.0f, 1.0f, &g_ui.runtime->motion_max_speed, 1},
        {"HEADING JITTER", 0.0f, 180.0f, 1.0f, &g_ui.runtime->motion_jitter_deg_per_sec, 2},
        {"BOUNCE MARGIN", 0.0f, fminf(g_ui.runtime->world_width_px, g_ui.runtime->world_height_px) * 0.5f, 1.0f, &g_ui.runtime->motion_bounce_margin, 3},
        {"SPAWN SPEED MEAN", 0.0f, 200.0f, 1.0f, &g_ui.runtime->motion_spawn_speed_mean, 4},
        {"SPAWN SPEED STD", 0.0f, 120.0f, 1.0f, &g_ui.runtime->motion_spawn_speed_std, 5},
    };

    sliders[4].min_value = g_ui.runtime->motion_min_speed;
    sliders[4].max_value = g_ui.runtime->motion_max_speed;
    if (sliders[4].max_value < sliders[4].min_value) {
        sliders[4].max_value = sliders[4].min_value;
    }

    float slider_x = text_x;
    float slider_width = content_width;

    for (size_t i = 0; i < sizeof(sliders)/sizeof(sliders[0]); ++i) {
        SliderSpec *spec = &sliders[i];
        ui_draw_text(slider_x, cursor_y, spec->label, text);
        panel_max_x = fmaxf(panel_max_x, slider_x + ui_measure_text(spec->label));
        UiRect slider_rect = {slider_x, cursor_y + 18.0f, slider_width, UI_SLIDER_HEIGHT};
        bool hovered = ui_rect_contains(&slider_rect, g_ui.mouse_x, g_ui.mouse_y);

        ui_add_rect(slider_rect.x, slider_rect.y, slider_rect.w, slider_rect.h, ui_color_rgba(0.15f, 0.15f, 0.18f, 0.95f));
        panel_max_x = fmaxf(panel_max_x, slider_rect.x + slider_rect.w);

        float range = spec->max_value - spec->min_value;
        float ratio = (range > 0.0f) ? ((*spec->value - spec->min_value) / range) : 0.0f;
        ratio = ui_clampf(ratio, 0.0f, 1.0f);
        float fill_w = slider_rect.w * ratio;
        UiColor track = hovered ? ui_color_rgba(0.2f, 0.4f, 0.7f, 1.0f) : ui_color_rgba(0.25f, 0.25f, 0.3f, 1.0f);
        ui_add_rect(slider_rect.x, slider_rect.y, fill_w, slider_rect.h, track);

        float knob_x = slider_rect.x + fill_w - 6.0f;
        ui_add_rect(knob_x, slider_rect.y - 2.0f, 12.0f, slider_rect.h + 4.0f, ui_color_rgba(0.9f, 0.9f, 0.9f, 1.0f));

        bool this_active = (g_ui.active_slider == spec->id);
        if (mouse_pressed && hovered) {
            g_ui.active_slider = spec->id;
            g_ui.capturing_mouse = true;
            this_active = true;
        }
        if (this_active) {
            if (mouse_down) {
                float t = (g_ui.mouse_x - slider_rect.x) / slider_rect.w;
                t = ui_clampf(t, 0.0f, 1.0f);
                float new_value = spec->min_value + t * range;
                if (spec->step > 0.0f && range > 0.0f) {
                    float steps = roundf((new_value - spec->min_value) / spec->step);
                    new_value = spec->min_value + steps * spec->step;
                }
                new_value = ui_clampf(new_value, spec->min_value, spec->max_value);
                if (fabsf(new_value - *spec->value) > 0.0001f) {
                    *spec->value = new_value;
                }
            } else {
                g_ui.active_slider = -1;
                g_ui.capturing_mouse = false;
            }
        }

        char buffer[32];
        if (spec->value == &g_ui.runtime->motion_spawn_speed_mean) {
            float min_allowed = g_ui.runtime->motion_min_speed;
            float max_allowed = g_ui.runtime->motion_max_speed;
            if (max_allowed < min_allowed) {
                max_allowed = min_allowed;
            }
            *spec->value = ui_clampf(*spec->value, min_allowed, max_allowed);
        } else if (spec->value == &g_ui.runtime->motion_spawn_speed_std) {
            if (*spec->value < 0.0f) {
                *spec->value = 0.0f;
            }
        }
        snprintf(buffer, sizeof(buffer), "%.1f", *spec->value);
        float value_x = slider_rect.x + slider_rect.w + 10.0f;
        ui_draw_text(value_x, slider_rect.y - 2.0f, buffer, text);
        panel_max_x = fmaxf(panel_max_x, value_x + ui_measure_text(buffer));
        cursor_y += UI_SLIDER_SPACING;
    }

    if (g_ui.runtime->motion_min_speed > g_ui.runtime->motion_max_speed) {
        g_ui.runtime->motion_max_speed = g_ui.runtime->motion_min_speed;
    }
    g_ui.runtime->motion_spawn_speed_mean = ui_clampf(g_ui.runtime->motion_spawn_speed_mean,
                                                   g_ui.runtime->motion_min_speed,
                                                   g_ui.runtime->motion_max_speed);
    if (g_ui.runtime->motion_spawn_speed_std < 0.0f) {
        g_ui.runtime->motion_spawn_speed_std = 0.0f;
    }

    ui_draw_text(text_x, cursor_y, "SPAWN MODE", text);
    panel_max_x = fmaxf(panel_max_x, text_x + ui_measure_text("SPAWN MODE"));
    cursor_y += 20.0f;
    float button_w = (content_width - 10.0f) * 0.5f;
    UiRect uniform_rect = {text_x, cursor_y, button_w, 28.0f};
    UiRect gaussian_rect = {text_x + button_w + 10.0f, cursor_y, button_w, 28.0f};
    bool uniform_active = g_ui.runtime->motion_spawn_mode == SPAWN_VELOCITY_UNIFORM_DIR;
    bool gaussian_active = g_ui.runtime->motion_spawn_mode == SPAWN_VELOCITY_GAUSSIAN_DIR;
    ui_add_rect(uniform_rect.x, uniform_rect.y, uniform_rect.w, uniform_rect.h,
                uniform_active ? accent : ui_color_rgba(0.2f, 0.2f, 0.25f, 1.0f));
    ui_add_rect(gaussian_rect.x, gaussian_rect.y, gaussian_rect.w, gaussian_rect.h,
                gaussian_active ? accent : ui_color_rgba(0.2f, 0.2f, 0.25f, 1.0f));
    panel_max_x = fmaxf(panel_max_x, gaussian_rect.x + gaussian_rect.w);
    ui_draw_text(uniform_rect.x + 8.0f, uniform_rect.y + 6.0f, "UNIFORM", text);
    ui_draw_text(gaussian_rect.x + 8.0f, gaussian_rect.y + 6.0f, "GAUSSIAN", text);
    if (mouse_pressed) {
        if (ui_rect_contains(&uniform_rect, g_ui.mouse_x, g_ui.mouse_y)) {
            g_ui.runtime->motion_spawn_mode = SPAWN_VELOCITY_UNIFORM_DIR;
        } else if (ui_rect_contains(&gaussian_rect, g_ui.mouse_x, g_ui.mouse_y)) {
            g_ui.runtime->motion_spawn_mode = SPAWN_VELOCITY_GAUSSIAN_DIR;
        }
    }
    cursor_y += 40.0f;

    ui_draw_text(text_x, cursor_y, "BEE COUNT", text);
    panel_max_x = fmaxf(panel_max_x, text_x + ui_measure_text("BEE COUNT"));
    cursor_y += 22.0f;
    UiRect minus_rect = {text_x, cursor_y, 28.0f, 24.0f};
    UiRect plus_rect = {text_x + 120.0f, cursor_y, 28.0f, 24.0f};
    ui_add_rect(minus_rect.x, minus_rect.y, minus_rect.w, minus_rect.h, ui_color_rgba(0.2f, 0.2f, 0.25f, 1.0f));
    ui_add_rect(plus_rect.x, plus_rect.y, plus_rect.w, plus_rect.h, ui_color_rgba(0.2f, 0.2f, 0.25f, 1.0f));
    panel_max_x = fmaxf(panel_max_x, plus_rect.x + plus_rect.w);
    ui_draw_text(minus_rect.x + 9.0f, minus_rect.y + 4.0f, "-", text);
    ui_draw_text(plus_rect.x + 7.0f, plus_rect.y + 4.0f, "+", text);

    if (mouse_pressed && ui_rect_contains(&minus_rect, g_ui.mouse_x, g_ui.mouse_y)) {
        if (g_ui.runtime->bee_count > 1) {
            g_ui.runtime->bee_count = g_ui.runtime->bee_count > 100 ? g_ui.runtime->bee_count - 100 : g_ui.runtime->bee_count - 1;
        }
    }
    if (mouse_pressed && ui_rect_contains(&plus_rect, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.runtime->bee_count += (g_ui.runtime->bee_count >= 100 ? 100 : 1);
        if (g_ui.runtime->bee_count > 1000000) {
            g_ui.runtime->bee_count = 1000000;
        }
    }

    char bee_buf[32];
    snprintf(bee_buf, sizeof(bee_buf), "%zu", g_ui.runtime->bee_count);
    ui_draw_text(text_x + 40.0f, cursor_y + 4.0f, bee_buf, text);
    panel_max_x = fmaxf(panel_max_x, text_x + 40.0f + ui_measure_text(bee_buf));
    cursor_y += 36.0f;

    ui_draw_text(text_x, cursor_y, "WORLD SIZE", text);
    panel_max_x = fmaxf(panel_max_x, text_x + ui_measure_text("WORLD SIZE"));
    cursor_y += 24.0f;
    UiRect world_minus_w = {text_x, cursor_y, 28.0f, 24.0f};
    UiRect world_plus_w = {text_x + 120.0f, cursor_y, 28.0f, 24.0f};
    UiRect world_minus_h = {text_x, cursor_y + 32.0f, 28.0f, 24.0f};
    UiRect world_plus_h = {text_x + 120.0f, cursor_y + 32.0f, 28.0f, 24.0f};

    ui_add_rect(world_minus_w.x, world_minus_w.y, world_minus_w.w, world_minus_w.h, ui_color_rgba(0.2f, 0.2f, 0.25f, 1.0f));
    ui_add_rect(world_plus_w.x, world_plus_w.y, world_plus_w.w, world_plus_w.h, ui_color_rgba(0.2f, 0.2f, 0.25f, 1.0f));
    ui_add_rect(world_minus_h.x, world_minus_h.y, world_minus_h.w, world_minus_h.h, ui_color_rgba(0.2f, 0.2f, 0.25f, 1.0f));
    ui_add_rect(world_plus_h.x, world_plus_h.y, world_plus_h.w, world_plus_h.h, ui_color_rgba(0.2f, 0.2f, 0.25f, 1.0f));
    panel_max_x = fmaxf(panel_max_x, fmaxf(world_plus_w.x + world_plus_w.w, world_plus_h.x + world_plus_h.w));
    ui_draw_text(world_minus_w.x + 9.0f, world_minus_w.y + 4.0f, "-", text);
    ui_draw_text(world_plus_w.x + 7.0f, world_plus_w.y + 4.0f, "+", text);
    ui_draw_text(world_minus_h.x + 9.0f, world_minus_h.y + 4.0f, "-", text);
    ui_draw_text(world_plus_h.x + 7.0f, world_plus_h.y + 4.0f, "+", text);

    if (mouse_pressed && ui_rect_contains(&world_minus_w, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.runtime->world_width_px = fmaxf(100.0f, g_ui.runtime->world_width_px - 100.0f);
    }
    if (mouse_pressed && ui_rect_contains(&world_plus_w, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.runtime->world_width_px += 100.0f;
    }
    if (mouse_pressed && ui_rect_contains(&world_minus_h, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.runtime->world_height_px = fmaxf(100.0f, g_ui.runtime->world_height_px - 100.0f);
    }
    if (mouse_pressed && ui_rect_contains(&world_plus_h, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.runtime->world_height_px += 100.0f;
    }

    char world_buf[48];
    snprintf(world_buf, sizeof(world_buf), "W %.0f", g_ui.runtime->world_width_px);
    ui_draw_text(text_x + 40.0f, cursor_y + 4.0f, world_buf, text);
    panel_max_x = fmaxf(panel_max_x, text_x + 40.0f + ui_measure_text(world_buf));
    snprintf(world_buf, sizeof(world_buf), "H %.0f", g_ui.runtime->world_height_px);
    ui_draw_text(text_x + 40.0f, cursor_y + 36.0f, world_buf, text);
    panel_max_x = fmaxf(panel_max_x, text_x + 40.0f + ui_measure_text(world_buf));
    cursor_y += 72.0f;

    UiRect pause_rect = {text_x, cursor_y, (content_width - 10.0f) * 0.5f, 28.0f};
    UiRect step_rect = {text_x + pause_rect.w + 10.0f, cursor_y, pause_rect.w, 28.0f};
    ui_add_rect(pause_rect.x, pause_rect.y, pause_rect.w, pause_rect.h, accent);
    ui_add_rect(step_rect.x, step_rect.y, step_rect.w, step_rect.h, ui_color_rgba(0.3f, 0.3f, 0.35f, 1.0f));
    panel_max_x = fmaxf(panel_max_x, step_rect.x + step_rect.w);
    ui_draw_text(pause_rect.x + 8.0f, pause_rect.y + 6.0f, g_ui.sim_paused ? "RESUME" : "PAUSE", text);
    ui_draw_text(step_rect.x + 8.0f, step_rect.y + 6.0f, "STEP", text);
    if (mouse_pressed && ui_rect_contains(&pause_rect, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.action_toggle_pause = true;
    }
    if (mouse_pressed && ui_rect_contains(&step_rect, g_ui.mouse_x, g_ui.mouse_y)) {
        g_ui.action_step = true;
    }
    cursor_y += 40.0f;

    bool dirty_now = false;
    const Params *runtime = g_ui.runtime;
    const Params *baseline = &g_ui.baseline;
    if (fabsf(runtime->motion_min_speed - baseline->motion_min_speed) > 0.0001f ||
        fabsf(runtime->motion_max_speed - baseline->motion_max_speed) > 0.0001f ||
        fabsf(runtime->motion_jitter_deg_per_sec - baseline->motion_jitter_deg_per_sec) > 0.0001f ||
        fabsf(runtime->motion_bounce_margin - baseline->motion_bounce_margin) > 0.0001f ||
        fabsf(runtime->motion_spawn_speed_mean - baseline->motion_spawn_speed_mean) > 0.0001f ||
        fabsf(runtime->motion_spawn_speed_std - baseline->motion_spawn_speed_std) > 0.0001f ||
        runtime->motion_spawn_mode != baseline->motion_spawn_mode ||
        runtime->bee_count != baseline->bee_count ||
        fabsf(runtime->world_width_px - baseline->world_width_px) > 0.0001f ||
        fabsf(runtime->world_height_px - baseline->world_height_px) > 0.0001f) {
        dirty_now = true;
    }
    g_ui.dirty = dirty_now;
    g_ui.reinit_required = (runtime->bee_count != baseline->bee_count) ||
                           fabsf(runtime->world_width_px - baseline->world_width_px) > 0.0001f ||
                           fabsf(runtime->world_height_px - baseline->world_height_px) > 0.0001f;

    UiRect apply_rect = {text_x, cursor_y, content_width, 30.0f};
    UiRect reset_rect = {text_x, cursor_y + 40.0f, content_width, 30.0f};
    UiColor apply_color = g_ui.dirty ? accent : ui_color_rgba(0.3f, 0.3f, 0.35f, 1.0f);
    ui_add_rect(apply_rect.x, apply_rect.y, apply_rect.w, apply_rect.h, apply_color);
    ui_draw_text(apply_rect.x + 8.0f, apply_rect.y + 8.0f, "APPLY", text);
    ui_add_rect(reset_rect.x, reset_rect.y, reset_rect.w, reset_rect.h, ui_color_rgba(0.25f, 0.25f, 0.30f, 1.0f));
    ui_draw_text(reset_rect.x + 8.0f, reset_rect.y + 8.0f, "RESET", text);
    panel_max_x = fmaxf(panel_max_x, reset_rect.x + reset_rect.w);

    if (mouse_pressed && ui_rect_contains(&apply_rect, g_ui.mouse_x, g_ui.mouse_y) && g_ui.dirty) {
        g_ui.action_apply = true;
        g_ui.action_reinit = g_ui.reinit_required;
    }
    if (mouse_pressed && ui_rect_contains(&reset_rect, g_ui.mouse_x, g_ui.mouse_y)) {
        *g_ui.runtime = g_ui.baseline;
        g_ui.dirty = false;
        g_ui.reinit_required = false;
        g_ui.action_reset = true;
        g_ui.action_apply = true;
        g_ui.action_reinit = false;
    }

    if (g_ui.reinit_required) {
        ui_draw_text(text_x, reset_rect.y + 40.0f, "REINIT REQUIRED", text);
        panel_max_x = fmaxf(panel_max_x, text_x + ui_measure_text("REINIT REQUIRED"));
    }

    panel_rect.h = (reset_rect.y + 80.0f) - panel_rect.y;
    panel_rect.w = fmaxf(UI_PANEL_WIDTH, (panel_max_x - panel_rect.x) + 20.0f);
    ui_update_rect(panel_bg_start, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h);
    ui_update_rect(panel_border_start, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h);
    g_ui.mouse_over_panel = ui_rect_contains(&panel_rect, g_ui.mouse_x, g_ui.mouse_y);
    g_ui.wants_mouse = g_ui.capturing_mouse || g_ui.mouse_over_panel;
    g_ui.wants_keyboard = true;

    if (g_ui.active_slider >= 0 && !mouse_down) {
        g_ui.active_slider = -1;
        g_ui.capturing_mouse = false;
    }

    g_ui.prev_mouse_down = mouse_down;
}

UiActions ui_update(const Input *input, bool sim_paused, float dt_sec) {
    (void)dt_sec;
    g_ui.sim_paused = sim_paused;

    UiActions actions = {0};
    ui_begin_frame(input);

    if (!g_ui.has_params || !g_ui.runtime) {
        return actions;
    }

    if (g_ui.action_toggle_pause) {
        actions.toggle_pause = true;
    }
    if (g_ui.action_step) {
        actions.step_once = true;
    }
    if (g_ui.action_apply) {
        actions.apply = true;
        actions.reinit_required = g_ui.action_reinit;
    }
    if (g_ui.action_reset) {
        actions.reset = true;
    }

    return actions;
}

void ui_render(int framebuffer_width, int framebuffer_height) {
    if (!g_ui.vertices || g_ui.vert_count == 0 || !g_ui.program) {
        return;
    }

    glUseProgram(g_ui.program);
    glUniform2f(g_ui.resolution_uniform, (float)framebuffer_width, (float)framebuffer_height);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    glBindVertexArray(g_ui.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_ui.vbo);
    glBufferData(GL_ARRAY_BUFFER, g_ui.vert_count * sizeof(UiVertex), g_ui.vertices, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLint)g_ui.vert_count);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glUseProgram(0);
}

bool ui_wants_mouse(void) {
    return g_ui.wants_mouse;
}

bool ui_wants_keyboard(void) {
    return g_ui.wants_keyboard;
}
