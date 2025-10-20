#include "app.h"
#include <math.h>
#include "params.h"
#include "platform.h"
#include "render.h"
#include "sim.h"
#include "ui.h"

#include "util/log.h"

static Platform g_platform = {0};
static Render g_render = {0};
static Params g_params = {0};
static Params g_params_runtime = {0};
static SimState *g_sim = NULL;
static bool g_app_initialized = false;
static bool g_app_should_quit = false;
static RenderCamera g_camera = {{0.0f, 0.0f}, 1.0f};
static float g_default_zoom = 1.0f;
static float g_default_center_world[2] = {0.0f, 0.0f};
static int g_fb_width = 0;
static int g_fb_height = 0;
static float g_sim_fixed_dt = 1.0f / 120.0f;
static const double g_sim_max_accumulator = 0.25;
static float clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static void app_reset_camera(void) {
    g_camera.center_world[0] = g_default_center_world[0];
    g_camera.center_world[1] = g_default_center_world[1];
    g_camera.zoom = g_default_zoom;
}

static void app_recompute_world_defaults(void);
static bool app_apply_runtime_params(bool reinit_required);

static void app_update_camera(const Input *input, float dt_sec) {
    if (!input || g_fb_width <= 0 || g_fb_height <= 0) {
        return;
    }

    int zoom_steps = input->wheel_y;
    if (input->key_plus_pressed) {
        ++zoom_steps;
    }
    if (input->key_minus_pressed) {
        --zoom_steps;
    }

    const float zoom_step_ratio = 0.1f;
    const float zoom_min = 0.05f;
    const float zoom_max = 20.0f;
    if (zoom_steps != 0) {
        float zoom_multiplier = powf(1.0f + zoom_step_ratio, (float)zoom_steps);
        float zoom_before = g_camera.zoom;
        float zoom_after = clampf(zoom_before * zoom_multiplier, zoom_min, zoom_max);
        if (zoom_after != zoom_before) {
            float cursor_x = input->mouse_x_px;
            float cursor_y = input->mouse_y_px;
            float half_w = 0.5f * (float)g_fb_width;
            float half_h = 0.5f * (float)g_fb_height;
            float dx_px = cursor_x - half_w;
            float dy_px = cursor_y - half_h;
            float world_x = g_camera.center_world[0] + dx_px / zoom_before;
            float world_y = g_camera.center_world[1] + dy_px / zoom_before;
            g_camera.zoom = zoom_after;
            g_camera.center_world[0] = world_x - dx_px / zoom_after;
            g_camera.center_world[1] = world_y - dy_px / zoom_after;
        }
    }

    if (input->key_reset_pressed) {
        app_reset_camera();
    }

    if (input->mouse_right_down) {
        g_camera.center_world[0] -= input->mouse_dx_px / g_camera.zoom;
        g_camera.center_world[1] -= input->mouse_dy_px / g_camera.zoom;
    }

    const float pan_speed_px_per_sec = 600.0f;
    float keyboard_dx_px = 0.0f;
    float keyboard_dy_px = 0.0f;
    if (input->key_d_down) {
        keyboard_dx_px += pan_speed_px_per_sec * dt_sec;
    }
    if (input->key_a_down) {
        keyboard_dx_px -= pan_speed_px_per_sec * dt_sec;
    }
    if (input->key_s_down) {
        keyboard_dy_px += pan_speed_px_per_sec * dt_sec;
    }
    if (input->key_w_down) {
        keyboard_dy_px -= pan_speed_px_per_sec * dt_sec;
    }
    if (keyboard_dx_px != 0.0f || keyboard_dy_px != 0.0f) {
        g_camera.center_world[0] += keyboard_dx_px / g_camera.zoom;
        g_camera.center_world[1] += keyboard_dy_px / g_camera.zoom;
    }
}

static void app_recompute_world_defaults(void) {
    float world_w = g_params.world_width_px > 0.0f ? g_params.world_width_px : (float)g_fb_width;
    float world_h = g_params.world_height_px > 0.0f ? g_params.world_height_px : (float)g_fb_height;

    g_default_center_world[0] = world_w * 0.5f;
    g_default_center_world[1] = world_h * 0.5f;
    if (world_w <= 0.0f) {
        g_default_center_world[0] = 0.0f;
    }
    if (world_h <= 0.0f) {
        g_default_center_world[1] = 0.0f;
    }

    if (world_w > 0.0f && world_h > 0.0f && g_fb_width > 0 && g_fb_height > 0) {
        float fit_x = (float)g_fb_width / world_w;
        float fit_y = (float)g_fb_height / world_h;
        g_default_zoom = fit_x < fit_y ? fit_x : fit_y;
    } else {
        g_default_zoom = 1.0f;
    }
    if (g_default_zoom <= 0.0f) {
        g_default_zoom = 1.0f;
    }
}

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
    g_params_runtime = g_params;
    if (g_params.sim_fixed_dt > 0.0f) {
        g_sim_fixed_dt = g_params.sim_fixed_dt;
    } else {
        g_sim_fixed_dt = 1.0f / 120.0f;
    }
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

    ui_init();
    ui_sync_to_params(&g_params, &g_params_runtime);

    if (!sim_init(&g_sim, &g_params)) {
        LOG_ERROR("Simulation initialization failed");
        ui_shutdown();
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

    g_fb_width = init_fb_w > 0 ? init_fb_w : g_params.window_width_px;
    g_fb_height = init_fb_h > 0 ? init_fb_h : g_params.window_height_px;
    if (g_fb_width <= 0) {
        g_fb_width = g_params.window_width_px;
    }
    if (g_fb_height <= 0) {
        g_fb_height = g_params.window_height_px;
    }
    app_recompute_world_defaults();
    app_reset_camera();

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

static bool app_apply_runtime_params(bool reinit_required) {
    Params new_params = g_params_runtime;
    char err[256];
    if (!params_validate(&new_params, err, sizeof err)) {
        LOG_WARN("runtime params invalid: %s", err);
        g_params_runtime = g_params;
        ui_sync_to_params(&g_params, &g_params_runtime);
        return false;
    }

    bool world_changed =
        fabsf(new_params.world_width_px - g_params.world_width_px) > 0.0001f ||
        fabsf(new_params.world_height_px - g_params.world_height_px) > 0.0001f;

    if (reinit_required) {
        SimState *fresh = NULL;
        if (!sim_init(&fresh, &new_params)) {
            LOG_ERROR("sim reinit failed; keeping previous simulation");
            g_params_runtime = g_params;
            ui_sync_to_params(&g_params, &g_params_runtime);
            return false;
        }
        sim_shutdown(g_sim);
        g_sim = fresh;
        g_sim_accumulator_sec = 0.0;
    } else if (g_sim) {
        sim_apply_runtime_params(g_sim, &new_params);
    }

    render_set_clear_color(&g_render, new_params.clear_color_rgba);

    g_params = new_params;
    if (g_params.sim_fixed_dt > 0.0f) {
        g_sim_fixed_dt = g_params.sim_fixed_dt;
    }

    if (reinit_required || world_changed) {
        app_recompute_world_defaults();
        app_reset_camera();
    }

    g_params_runtime = g_params;
    ui_sync_to_params(&g_params, &g_params_runtime);
    LOG_INFO("ui: applied params (reinit=%d)", reinit_required ? 1 : 0);
    return true;
}

void app_frame(void) {
    if (!g_app_initialized) {
        return;
    }

    Input input = (Input){0};
    Timing timing = (Timing){0};
    plat_pump(&g_platform, &input, &timing);

    UiActions ui_actions = ui_update(&input, g_sim_paused, timing.dt_sec);
    bool ui_mouse = ui_wants_mouse();
    bool ui_keyboard = ui_wants_keyboard();

    if (input.quit_requested) {
        g_app_should_quit = true;
    }

    if (ui_actions.apply) {
        app_apply_runtime_params(ui_actions.reinit_required);
    }
    if (ui_actions.reset) {
        LOG_INFO("ui: runtime params reset to baseline");
    }

    bool toggle_pause = ui_actions.toggle_pause;
    if (!ui_keyboard && input.key_space_pressed) {
        toggle_pause = true;
    }
    if (toggle_pause) {
        g_sim_paused = !g_sim_paused;
        LOG_INFO("pause=%d", g_sim_paused ? 1 : 0);
    }

    bool step_requested = false;
    if (ui_actions.step_once) {
        step_requested = true;
    }
    if (!ui_keyboard && input.key_period_pressed) {
        step_requested = true;
    }
    step_requested = step_requested && g_sim_paused;

    Input camera_input = input;
    if (ui_mouse) {
        camera_input.mouse_right_down = false;
        camera_input.mouse_right_pressed = false;
        camera_input.mouse_dx_px = 0.0f;
        camera_input.mouse_dy_px = 0.0f;
        camera_input.wheel_y = 0;
    }
    if (ui_keyboard) {
        camera_input.key_plus_pressed = false;
        camera_input.key_minus_pressed = false;
        camera_input.key_plus_down = false;
        camera_input.key_minus_down = false;
        camera_input.key_reset_pressed = false;
        camera_input.key_w_down = false;
        camera_input.key_a_down = false;
        camera_input.key_s_down = false;
        camera_input.key_d_down = false;
    }

    app_update_camera(&camera_input, timing.dt_sec);

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
        if (fb_w > 0) {
            g_fb_width = fb_w;
        }
        if (fb_h > 0) {
            g_fb_height = fb_h;
        }
        app_recompute_world_defaults();
    }

    RenderView view = (RenderView){0};
    if (g_sim) {
        view = sim_build_view(g_sim);
    }
    render_set_camera(&g_render, &g_camera);
    render_frame(&g_render, &view);
    ui_render(g_fb_width, g_fb_height);
    plat_swap(&g_platform);
}

void app_shutdown(void) {
    if (!g_app_initialized) {
        return;
    }

    sim_shutdown(g_sim);
    g_sim = NULL;
    ui_shutdown();
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
    app_reset_camera();
}

bool app_should_quit(void) {
    return g_app_should_quit;
}

