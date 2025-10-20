#include "app.h"
#include "params.h"
#include "platform.h"
#include "render.h"
#include "sim.h"
#include "util/log.h"

static Platform g_platform = {0};
static Render g_render = {0};
static Params g_params = {0};
static SimState *g_sim = NULL;
static bool g_app_initialized = false;
static bool g_app_should_quit = false;
static const float g_sim_fixed_dt = 1.0f / 120.0f;
static const double g_sim_max_accumulator = 0.25;
static double g_sim_accumulator_sec = 0.0;
static bool g_sim_paused = false;
static double g_log_accumulator_sec = 0.0;
static unsigned g_log_frame_counter = 0;
static unsigned g_log_tick_counter = 0;

bool app_init(const Params *params) {
    if (g_app_initialized) {
        LOG_WARN("app_init called twice; ignoring subsequent call");
        return true;
    }

    log_init();
    log_set_level(LOG_LEVEL_INFO);

    if (!params) {
        LOG_ERROR("app_init received null Params pointer");
        return false;
    }

    g_params = *params;
    char err[256];
    if (!params_validate(&g_params, err, sizeof err)) {
        LOG_ERROR("Params validation failed: %s", err);
        return false;
    }

    LOG_INFO("=== Bee Hive Boot ===");
    LOG_INFO("Window: %dx%d \"%s\" (vsync %s)",
             g_params.window_width_px,
             g_params.window_height_px,
             g_params.window_title,
             g_params.vsync_on ? "on" : "off");
    LOG_INFO("Render: clear rgba=(%.2f, %.2f, %.2f, %.2f) bee_radius=%.2f seed=0x%llx",
             g_params.clear_color_rgba[0],
             g_params.clear_color_rgba[1],
             g_params.clear_color_rgba[2],
             g_params.clear_color_rgba[3],
             g_params.bee_radius_px,
             (unsigned long long)g_params.rng_seed);
    LOG_INFO("Bee color rgba=(%.2f, %.2f, %.2f, %.2f)",
             g_params.bee_color_rgba[0],
             g_params.bee_color_rgba[1],
             g_params.bee_color_rgba[2],
             g_params.bee_color_rgba[3]);
    LOG_INFO("Sim: bees=%zu world=(%.0f x %.0f)px",
             g_params.bee_count,
             g_params.world_width_px,
             g_params.world_height_px);

    if (!plat_init(&g_platform, &g_params)) {
        LOG_ERROR("Platform initialization failed");
        plat_shutdown(&g_platform);
        return false;
    }

    if (!render_init(&g_render, &g_params)) {
        LOG_ERROR("Render initialization failed");
        plat_shutdown(&g_platform);
        return false;
    }

    if (!sim_init(&g_sim, &g_params)) {
        LOG_ERROR("Simulation initialization failed");
        render_shutdown(&g_render);
        plat_shutdown(&g_platform);
        return false;
    }

    int init_fb_w = g_params.window_width_px;
    int init_fb_h = g_params.window_height_px;
    if (plat_poll_resize(&g_platform, &init_fb_w, &init_fb_h)) {
        LOG_INFO("Framebuffer initial size: %dx%d", init_fb_w, init_fb_h);
    }
    render_resize(&g_render, init_fb_w, init_fb_h);

    g_sim_accumulator_sec = 0.0;
    g_sim_paused = false;
    g_log_accumulator_sec = 0.0;
    g_log_frame_counter = 0;
    g_log_tick_counter = 0;

    g_app_initialized = true;
    g_app_should_quit = false;
    LOG_INFO("fixed_dt=%.5f vsync=%d", g_sim_fixed_dt, g_params.vsync_on ? 1 : 0);
    LOG_INFO("Boot ok");
    return true;
}

void app_frame(void) {
    if (!g_app_initialized) {
        return;
    }

    Input input = {0};
    Timing timing = {0};
    plat_pump(&g_platform, &input, &timing);

    if (input.quit_requested) {
        g_app_should_quit = true;
    }

    if (input.key_space_pressed) {
        g_sim_paused = !g_sim_paused;
        LOG_INFO("pause=%d", g_sim_paused ? 1 : 0);
    }

    bool step_requested = input.key_period_pressed && g_sim_paused;

    if (!g_sim_paused) {
        g_sim_accumulator_sec += timing.dt_sec;
        if (g_sim_accumulator_sec > g_sim_max_accumulator) {
            g_sim_accumulator_sec = g_sim_max_accumulator;
        }
    }

    unsigned ticks_this_frame = 0;
    if (g_sim) {
        if (g_sim_paused) {
            if (step_requested) {
                sim_tick(g_sim, g_sim_fixed_dt);
                ticks_this_frame = 1;
                LOG_INFO("step one tick (%.3fms)", g_sim_fixed_dt * 1000.0f);
            }
        } else {
            while (g_sim_accumulator_sec >= (double)g_sim_fixed_dt) {
                sim_tick(g_sim, g_sim_fixed_dt);
                g_sim_accumulator_sec -= (double)g_sim_fixed_dt;
                ++ticks_this_frame;
            }
            if (g_sim_accumulator_sec < 0.0) {
                g_sim_accumulator_sec = 0.0;
            }
        }
    }

    g_log_accumulator_sec += timing.dt_sec;
    g_log_frame_counter += 1;
    g_log_tick_counter += ticks_this_frame;

    if (g_log_accumulator_sec >= 1.0) {
        if (g_sim_paused) {
            LOG_INFO("paused (press '.' to step)");
        } else {
            double dt_ms = timing.dt_sec * 1000.0;
            double acc_ms = g_sim_accumulator_sec * 1000.0;
            double fps_f = g_log_accumulator_sec > 0.0
                               ? (double)g_log_frame_counter / g_log_accumulator_sec
                               : 0.0;
            int fps_est = (int)(fps_f + 0.5);
            LOG_INFO("dt=%.3fms acc=%.2fms ticks=%u fps~%d",
                     dt_ms,
                     acc_ms,
                     g_log_tick_counter,
                     fps_est);
        }
        g_log_accumulator_sec = 0.0;
        g_log_frame_counter = 0;
        g_log_tick_counter = 0;
    }

    int fb_w = 0;
    int fb_h = 0;
    if (plat_poll_resize(&g_platform, &fb_w, &fb_h)) {
        LOG_INFO("Framebuffer resized to %dx%d", fb_w, fb_h);
        render_resize(&g_render, fb_w, fb_h);
    }

    RenderView view = {0};
    if (g_sim) {
        view = sim_build_view(g_sim);
    }
    render_frame(&g_render, &view);
    plat_swap(&g_platform);
}

void app_shutdown(void) {
    if (!g_app_initialized) {
        return;
    }

    sim_shutdown(g_sim);
    g_sim = NULL;
    render_shutdown(&g_render);
    plat_shutdown(&g_platform);
    log_shutdown();

    g_app_should_quit = false;
    g_app_initialized = false;
    g_sim_paused = false;
    g_sim_accumulator_sec = 0.0;
    g_log_accumulator_sec = 0.0;
    g_log_frame_counter = 0;
    g_log_tick_counter = 0;
}

bool app_should_quit(void) {
    return g_app_should_quit;
}
