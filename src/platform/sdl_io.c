#include "platform.h"

#include <SDL.h>
#include <glad/glad.h>

#include <stdlib.h>

#include "params.h"
#include "util/log.h"

typedef struct PlatformState {
    SDL_Window *window;
    SDL_GLContext gl_context;
    Uint64 ticks_prev;
    double inv_freq;
    bool vsync_enabled;
    int drawable_w;
    int drawable_h;
    bool prev_key_escape_down;
    bool prev_key_space_down;
    bool prev_key_period_down;
} PlatformState;

static void platform_log_gl_info(bool vsync_requested) {
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    LOG_INFO("OpenGL: %s", version ? version : "(unknown)");
    LOG_INFO("Vendor : %s", vendor ? vendor : "(unknown)");
    LOG_INFO("Renderer: %s", renderer ? renderer : "(unknown)");
    const int swap_interval = SDL_GL_GetSwapInterval();
    LOG_INFO("Swap interval: %d (requested %s)",
             swap_interval,
             vsync_requested ? "vsync on" : "vsync off");
}

static void platform_cleanup_partial(PlatformState *state) {
    if (!state) {
        return;
    }
    if (state->gl_context) {
        SDL_GL_DeleteContext(state->gl_context);
    }
    if (state->window) {
        SDL_DestroyWindow(state->window);
    }
    free(state);

    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    if (SDL_WasInit(0) != 0) {
        SDL_Quit();
    }
}

bool plat_init(Platform *plat, const Params *params) {
    if (!plat || !params) {
        LOG_ERROR("plat_init received null argument");
        return false;
    }
    if (plat->state) {
        LOG_WARN("plat_init called with non-null state; shutting down previous instance");
        plat_shutdown(plat);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    const Uint32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    SDL_Window *window = SDL_CreateWindow(
        params->window_title,
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        params->window_width_px,
        params->window_height_px,
        window_flags);
    if (!window) {
        LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        SDL_Quit();
        return false;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        LOG_ERROR("SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        SDL_Quit();
        return false;
    }

    if (SDL_GL_MakeCurrent(window, gl_ctx) != 0) {
        LOG_ERROR("SDL_GL_MakeCurrent failed: %s", SDL_GetError());
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        SDL_Quit();
        return false;
    }

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        LOG_ERROR("gladLoadGLLoader failed");
        SDL_GL_MakeCurrent(window, NULL);
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        SDL_Quit();
        return false;
    }

    if (SDL_GL_SetSwapInterval(params->vsync_on ? 1 : 0) != 0) {
        LOG_WARN("SDL_GL_SetSwapInterval(%d) failed: %s",
                 params->vsync_on ? 1 : 0,
                 SDL_GetError());
    }

    PlatformState *state = (PlatformState *)calloc(1, sizeof(PlatformState));
    if (!state) {
        LOG_ERROR("Failed to allocate PlatformState");
        SDL_GL_MakeCurrent(window, NULL);
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        SDL_Quit();
        return false;
    }

    state->window = window;
    state->gl_context = gl_ctx;
    state->ticks_prev = SDL_GetPerformanceCounter();
    state->inv_freq = 1.0 / (double)SDL_GetPerformanceFrequency();
    state->vsync_enabled = SDL_GL_GetSwapInterval() > 0;
    plat->state = state;

    platform_log_gl_info(params->vsync_on);

    return true;
}

void plat_pump(Platform *plat, Input *out_input, Timing *out_timing) {
    if (!plat || !plat->state) {
        return;
    }
    PlatformState *state = (PlatformState *)plat->state;

    bool quit_requested = false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                quit_requested = true;
                break;
            default:
                break;
        }
    }

    const Uint8 *keyboard = SDL_GetKeyboardState(NULL);
    bool escape_down = keyboard ? keyboard[SDL_SCANCODE_ESCAPE] != 0 : false;
    bool space_down = keyboard ? keyboard[SDL_SCANCODE_SPACE] != 0 : false;
    bool period_down = keyboard ? keyboard[SDL_SCANCODE_PERIOD] != 0 : false;

    bool escape_pressed = escape_down && !state->prev_key_escape_down;
    bool space_pressed = space_down && !state->prev_key_space_down;
    bool period_pressed = period_down && !state->prev_key_period_down;

    state->prev_key_escape_down = escape_down;
    state->prev_key_space_down = space_down;
    state->prev_key_period_down = period_down;

    Input input = {0};
    input.key_escape_down = escape_down;
    input.key_space_down = space_down;
    input.key_period_down = period_down;
    input.key_escape_pressed = escape_pressed;
    input.key_space_pressed = space_pressed;
    input.key_period_pressed = period_pressed;
    input.quit_requested = quit_requested || escape_pressed;

    Uint64 now_ticks = SDL_GetPerformanceCounter();
    Uint64 prev_ticks = state->ticks_prev;
    state->ticks_prev = now_ticks;

    double now_sec = (double)now_ticks * state->inv_freq;
    float dt_sec = 0.0f;
    if (prev_ticks != 0 && now_ticks >= prev_ticks) {
        dt_sec = (float)((now_ticks - prev_ticks) * state->inv_freq);
    }
    if (dt_sec < 0.0f) {
        dt_sec = 0.0f;
    }

    const float min_dt = 1.0f / 240.0f;
    const float max_dt = 0.1f;
    if (dt_sec < min_dt) {
        dt_sec = min_dt;
    }
    if (dt_sec > max_dt) {
        dt_sec = max_dt;
    }

    if (out_input) {
        *out_input = input;
    }
    if (out_timing) {
        out_timing->dt_sec = dt_sec;
        out_timing->now_sec = now_sec;
    }
}

void plat_swap(Platform *plat) {
    if (!plat || !plat->state) {
        return;
    }
    PlatformState *state = (PlatformState *)plat->state;
    SDL_GL_SwapWindow(state->window);
}

void plat_shutdown(Platform *plat) {
    if (!plat || !plat->state) {
        return;
    }
    PlatformState *state = (PlatformState *)plat->state;
    plat->state = NULL;

    SDL_GL_MakeCurrent(state->window, NULL);
    platform_cleanup_partial(state);
}

bool plat_poll_resize(Platform *plat, int *out_fb_w, int *out_fb_h) {
    if (!plat || !plat->state) {
        return false;
    }
    PlatformState *state = (PlatformState *)plat->state;
    int w = 0;
    int h = 0;
    SDL_GL_GetDrawableSize(state->window, &w, &h);
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (w == state->drawable_w && h == state->drawable_h) {
        return false;
    }
    state->drawable_w = w;
    state->drawable_h = h;
    if (out_fb_w) {
        *out_fb_w = w;
    }
    if (out_fb_h) {
        *out_fb_h = h;
    }
    return true;
}
