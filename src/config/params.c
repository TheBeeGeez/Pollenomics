#include "params.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "util/log.h"

static void copy_string(char *dst, size_t cap, const char *src) {
    if (cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
#if defined(_MSC_VER)
    strncpy_s(dst, cap, src, _TRUNCATE);
#else
    snprintf(dst, cap, "%s", src);
#endif
}

void params_init_defaults(Params *params) {
    if (!params) {
        return;
    }
    params->window_width_px = 1280;
    params->window_height_px = 720;
    copy_string(params->window_title, PARAMS_MAX_TITLE_CHARS, "Bee Simulation");
    params->vsync_on = true;
    params->clear_color_rgba[0] = 0.98f;
    params->clear_color_rgba[1] = 0.98f;
    params->clear_color_rgba[2] = 0.96f;
    params->clear_color_rgba[3] = 1.0f;
    params->bee_radius_px = 12.0f;
    params->bee_color_rgba[0] = 0.10f;
    params->bee_color_rgba[1] = 0.10f;
    params->bee_color_rgba[2] = 0.10f;
    params->bee_color_rgba[3] = 1.0f;
    params->bee_count = 256;
    params->world_width_px = (float)params->window_width_px;
    params->world_height_px = (float)params->window_height_px;
    params->sim_fixed_dt = 1.0f / 120.0f;
    params->motion_min_speed = 10.0f;
    params->motion_max_speed = 80.0f;
    params->motion_jitter_deg_per_sec = 15.0f;
    params->motion_bounce_margin = 0.0f;
    params->motion_spawn_speed_mean = 40.0f;
    params->motion_spawn_speed_std = 10.0f;
    params->motion_spawn_mode = SPAWN_VELOCITY_UNIFORM_DIR;
    params->rng_seed = UINT64_C(0xBEE);

    params->hive.rect_x = 200.0f;
    params->hive.rect_y = 200.0f;
    params->hive.rect_w = 400.0f;
    params->hive.rect_h = 260.0f;
    params->hive.entrance_side = 1;  // bottom
    params->hive.entrance_t = 0.5f;
    params->hive.entrance_width = 120.0f;
    params->hive.restitution = 0.8f;
    params->hive.tangent_damp = 0.9f;
    params->hive.max_resolve_iters = 2;
    params->hive.safety_margin = 0.5f;

    params->bee.harvest_rate_uLps = 18.0f;
    params->bee.capacity_uL = 45.0f;
    params->bee.unload_rate_uLps = 160.0f;
    params->bee.rest_recovery_per_s = 0.35f;
    params->bee.speed_mps = 60.0f;
    params->bee.seek_accel = 220.0f;
    params->bee.arrive_tol_world = params->bee_radius_px * 2.0f;
}

bool params_validate(const Params *params, char *err_buf, size_t err_cap) {
    if (!params) {
        if (err_buf && err_cap > 0) {
#if defined(_MSC_VER)
            strncpy_s(err_buf, err_cap, "Params pointer is null", _TRUNCATE);
#else
            snprintf(err_buf, err_cap, "%s", "Params pointer is null");
#endif
        }
        return false;
    }
    if (params->bee.harvest_rate_uLps <= 0.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "bee harvest_rate_uLps (%.2f) must be > 0",
                     params->bee.harvest_rate_uLps);
        }
        return false;
    }
    if (params->bee.capacity_uL <= 0.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "bee capacity_uL (%.2f) must be > 0",
                     params->bee.capacity_uL);
        }
        return false;
    }
    if (params->bee.unload_rate_uLps <= 0.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "bee unload_rate_uLps (%.2f) must be > 0",
                     params->bee.unload_rate_uLps);
        }
        return false;
    }
    if (params->bee.rest_recovery_per_s <= 0.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "bee rest_recovery_per_s (%.2f) must be > 0",
                     params->bee.rest_recovery_per_s);
        }
        return false;
    }
    if (params->bee.speed_mps <= 0.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "bee speed_mps (%.2f) must be > 0", params->bee.speed_mps);
        }
        return false;
    }
    if (params->bee.seek_accel <= 0.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "bee seek_accel (%.2f) must be > 0", params->bee.seek_accel);
        }
        return false;
    }
    if (params->bee.arrive_tol_world <= 0.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "bee arrive_tol_world (%.2f) must be > 0",
                     params->bee.arrive_tol_world);
        }
        return false;
    }

    if (params->window_width_px < 320) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "window_width_px (%d) must be >= 320",
                     params->window_width_px);
        }
        return false;
    }
    if (params->window_height_px < 240) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "window_height_px (%d) must be >= 240",
                     params->window_height_px);
        }
        return false;
    }
    if (params->window_title[0] == '\0') {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "%s", "window_title must not be empty");
        }
        return false;
    }
    if (params->bee_radius_px <= 0.0f || params->bee_radius_px > 256.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap,
                     "bee_radius_px (%f) must be within (0, 256]", params->bee_radius_px);
        }
        return false;
    }
    if (params->bee_count == 0 || params->bee_count > 1000000) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap,
                     "bee_count (%zu) must be within [1, 1000000]", params->bee_count);
        }
        return false;
    }
    if (params->world_width_px <= 0.0f || params->world_height_px <= 0.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap,
                     "world dimensions must be positive (got %f x %f)",
                     params->world_width_px, params->world_height_px);
        }
        return false;
    }
    if (params->sim_fixed_dt <= 0.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "sim_fixed_dt (%f) must be > 0", params->sim_fixed_dt);
        }
        return false;
    }
    if (params->motion_min_speed <= 0.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "motion_min_speed (%f) must be > 0", params->motion_min_speed);
        }
        return false;
    }
    if (params->motion_spawn_speed_mean <= 0.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap,
                     "motion_spawn_speed_mean (%f) must be > 0",
                     params->motion_spawn_speed_mean);
        }
        return false;
    }
    if (params->motion_max_speed < params->motion_min_speed) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap,
                     "motion_max_speed (%f) must be >= motion_min_speed (%f)",
                     params->motion_max_speed, params->motion_min_speed);
        }
        return false;
    }
    if (params->motion_jitter_deg_per_sec < 0.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap,
                     "motion_jitter_deg_per_sec (%f) must be >= 0",
                     params->motion_jitter_deg_per_sec);
        }
        return false;
    }
    if (params->motion_bounce_margin < 0.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap,
                     "motion_bounce_margin (%f) must be >= 0",
                     params->motion_bounce_margin);
        }
        return false;
    }
    if (params->motion_spawn_mode != SPAWN_VELOCITY_UNIFORM_DIR &&
        params->motion_spawn_mode != SPAWN_VELOCITY_GAUSSIAN_DIR) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap,
                     "motion_spawn_mode (%d) must be %d or %d",
                     params->motion_spawn_mode,
                     SPAWN_VELOCITY_UNIFORM_DIR,
                     SPAWN_VELOCITY_GAUSSIAN_DIR);
        }
        return false;
    }
    if (params->motion_spawn_speed_std < 0.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap,
                     "motion_spawn_speed_std (%f) must be >= 0",
                     params->motion_spawn_speed_std);
        }
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        const float c = params->clear_color_rgba[i];
        if (c < 0.0f || c > 1.0f) {
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap,
                         "clear_color_rgba[%d] (%f) must be within [0, 1]", i, c);
            }
            return false;
        }
    }
    for (int i = 0; i < 4; ++i) {
        const float c = params->bee_color_rgba[i];
        if (c < 0.0f || c > 1.0f) {
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap,
                         "bee_color_rgba[%d] (%f) must be within [0, 1]", i, c);
            }
            return false;
        }
    }

    bool hive_enabled = params->hive.rect_w > 0.0f && params->hive.rect_h > 0.0f;
    if (params->hive.rect_w < 0.0f || params->hive.rect_h < 0.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap,
                     "hive dimensions must be non-negative (got %.2f x %.2f)",
                     params->hive.rect_w, params->hive.rect_h);
        }
        return false;
    }
    if (hive_enabled) {
        if (params->hive.entrance_side < 0 || params->hive.entrance_side > 3) {
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap,
                         "hive entrance_side (%d) must be 0-3", params->hive.entrance_side);
            }
            return false;
        }
        if (params->hive.entrance_t < 0.0f || params->hive.entrance_t > 1.0f) {
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap,
                         "hive entrance_t (%.2f) must be within [0, 1]", params->hive.entrance_t);
            }
            return false;
        }
        if (params->hive.entrance_width <= 0.0f) {
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap, "hive entrance_width (%.2f) must be > 0",
                         params->hive.entrance_width);
            }
            return false;
        }
        float side_length = (params->hive.entrance_side == 2 || params->hive.entrance_side == 3)
                                ? params->hive.rect_h
                                : params->hive.rect_w;
        float max_radius = params->bee_radius_px;
        float required_clearance = 2.0f * max_radius;
        if (params->hive.entrance_width > side_length - required_clearance) {
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap,
                         "hive entrance_width (%.2f) must be <= side length minus 2*bee_radius (%.2f)",
                         params->hive.entrance_width, side_length - required_clearance);
            }
            return false;
        }
        if (params->hive.restitution < 0.0f || params->hive.restitution > 1.0f) {
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap,
                         "hive restitution (%.2f) must be within [0, 1]", params->hive.restitution);
            }
            return false;
        }
        if (params->hive.tangent_damp < 0.0f || params->hive.tangent_damp > 1.0f) {
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap,
                         "hive tangent_damp (%.2f) must be within [0, 1]",
                         params->hive.tangent_damp);
            }
            return false;
        }
        if (params->hive.max_resolve_iters < 0 || params->hive.max_resolve_iters > 8) {
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap,
                         "hive max_resolve_iters (%d) must be within [0, 8]",
                         params->hive.max_resolve_iters);
            }
            return false;
        }
        if (params->hive.safety_margin < 0.0f || params->hive.safety_margin > 5.0f) {
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap,
                         "hive safety_margin (%.2f) must be within [0, 5]",
                         params->hive.safety_margin);
            }
            return false;
        }
    }
    if (err_buf && err_cap > 0) {
        err_buf[0] = '\0';
    }
    return true;
}

bool params_load_from_json(const char *path, Params *out_params,
                           char *err_buf, size_t err_cap) {
    (void)path;
    (void)out_params;
    if (err_buf && err_cap > 0) {
        snprintf(err_buf, err_cap, "%s", "params_load_from_json not implemented yet");
    }
    LOG_WARN("params_load_from_json is not implemented; using defaults");
    return false;
}

