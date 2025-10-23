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

    params->hive.center_x = params->world_width_px * 0.5f;
    params->hive.center_y = params->world_height_px * 0.45f;
    params->hive.radius_tiles = 6;
    params->hive.storage_radius_tiles = 3;
    params->hive.entrance_dir = 3;  // south
    params->hive.entrance_width_tiles = 3;

    params->bee.harvest_rate_uLps = 18.0f;
    params->bee.capacity_uL = 45.0f;
    params->bee.unload_rate_uLps = 160.0f;
    params->bee.rest_recovery_per_s = 0.35f;
    params->bee.speed_mps = 60.0f;
    params->bee.seek_accel = 220.0f;
    params->bee.arrive_tol_world = params->bee_radius_px * 2.0f;

    params->hex.cell_radius = 42.0f;
    params->hex.q_min = -36;
    params->hex.q_max = 36;
    params->hex.r_min = -28;
    params->hex.r_max = 28;
    params->hex.origin_x = params->world_width_px * 0.5f;
    params->hex.origin_y = params->world_height_px * 0.5f;
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

    if (params->hive.radius_tiles < 0) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "hive radius_tiles (%d) must be >= 0",
                     params->hive.radius_tiles);
        }
        return false;
    }
    if (params->hive.storage_radius_tiles < 0) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "hive storage_radius_tiles (%d) must be >= 0",
                     params->hive.storage_radius_tiles);
        }
        return false;
    }
    if (params->hive.storage_radius_tiles > params->hive.radius_tiles) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap,
                     "hive storage_radius_tiles (%d) must be <= radius_tiles (%d)",
                     params->hive.storage_radius_tiles, params->hive.radius_tiles);
        }
        return false;
    }
    if (params->hive.radius_tiles > 0) {
        if (params->hive.entrance_dir < 0 || params->hive.entrance_dir > 5) {
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap,
                         "hive entrance_dir (%d) must be within [0, 5]",
                         params->hive.entrance_dir);
            }
            return false;
        }
        if (params->hive.entrance_width_tiles <= 0) {
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap,
                         "hive entrance_width_tiles (%d) must be > 0",
                         params->hive.entrance_width_tiles);
            }
            return false;
        }
    }
    if (params->hex.cell_radius <= 0.0f) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap,
                     "hex cell_radius (%.2f) must be > 0", params->hex.cell_radius);
        }
        return false;
    }
    int hex_width = params->hex.q_max - params->hex.q_min + 1;
    int hex_height = params->hex.r_max - params->hex.r_min + 1;
    if (hex_width <= 0 || hex_height <= 0) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap,
                     "hex axial bounds invalid q[%d,%d] r[%d,%d]",
                     params->hex.q_min,
                     params->hex.q_max,
                     params->hex.r_min,
                     params->hex.r_max);
        }
        return false;
    }
    size_t hex_tiles = (size_t)hex_width * (size_t)hex_height;
    if (hex_tiles > 500000) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap,
                     "hex tile count (%zu) exceeds limit (500000)", hex_tiles);
        }
        return false;
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

