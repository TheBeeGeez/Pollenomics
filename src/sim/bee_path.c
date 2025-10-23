#include "bee_path.h"

#include <float.h>
#include <math.h>

#include "sim_internal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float bee_path_orient(float ax, float ay, float bx, float by, float cx, float cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static bool bee_path_on_segment(float ax, float ay, float bx, float by, float px, float py) {
    const float eps = 1e-4f;
    return (px >= fminf(ax, bx) - eps && px <= fmaxf(ax, bx) + eps &&
            py >= fminf(ay, by) - eps && py <= fmaxf(ay, by) + eps);
}

static bool bee_path_segments_intersect(float ax,
                                        float ay,
                                        float bx,
                                        float by,
                                        float cx,
                                        float cy,
                                        float dx,
                                        float dy) {
    const float eps = 1e-5f;
    float o1 = bee_path_orient(ax, ay, bx, by, cx, cy);
    float o2 = bee_path_orient(ax, ay, bx, by, dx, dy);
    float o3 = bee_path_orient(cx, cy, dx, dy, ax, ay);
    float o4 = bee_path_orient(cx, cy, dx, dy, bx, by);

    if (fabsf(o1) < eps && bee_path_on_segment(ax, ay, bx, by, cx, cy)) {
        return true;
    }
    if (fabsf(o2) < eps && bee_path_on_segment(ax, ay, bx, by, dx, dy)) {
        return true;
    }
    if (fabsf(o3) < eps && bee_path_on_segment(cx, cy, dx, dy, ax, ay)) {
        return true;
    }
    if (fabsf(o4) < eps && bee_path_on_segment(cx, cy, dx, dy, bx, by)) {
        return true;
    }

    bool o12 = (o1 > eps && o2 < -eps) || (o1 < -eps && o2 > eps);
    bool o34 = (o3 > eps && o4 < -eps) || (o3 < -eps && o4 > eps);
    if (!o12 || !o34) {
        return false;
    }

    float abx = bx - ax;
    float aby = by - ay;
    float cdx = dx - cx;
    float cdy = dy - cy;
    float denom = abx * cdy - aby * cdx;
    if (fabsf(denom) < eps) {
        return false;
    }
    float t = ((cx - ax) * cdy - (cy - ay) * cdx) / denom;
    if (t <= eps || t >= 1.0f - eps) {
        return false;
    }
    return true;
}

static bool bee_path_hive_exists(const SimState *state) {
    return state && state->hex_world && hex_world_hive_enabled(state->hex_world);
}

static bool bee_path_point_inside_hive(const SimState *state, float x, float y) {
    if (!bee_path_hive_exists(state)) {
        return false;
    }
    size_t index = (size_t)SIZE_MAX;
    if (!hex_world_tile_from_world(state->hex_world, x, y, &index)) {
        return false;
    }
    if (index >= state->hex_world->tile_count) {
        return false;
    }
    HexTerrain terrain = state->hex_world->tiles[index].terrain;
    return (terrain == HEX_TERRAIN_HIVE_INTERIOR || terrain == HEX_TERRAIN_HIVE_STORAGE ||
            terrain == HEX_TERRAIN_HIVE_ENTRANCE);
}

static bool bee_path_point_inside_world(const SimState *state, float x, float y, float radius) {
    if (!state) {
        return true;
    }
    float r = radius > 0.0f ? radius : state->default_radius;
    if (r <= 0.0f) {
        r = 1.0f;
    }
    float min_x = r;
    float min_y = r;
    float max_x = state->world_w - r;
    float max_y = state->world_h - r;
    return (x >= min_x && x <= max_x && y >= min_y && y <= max_y);
}

static bool bee_path_segment_hits_hive(const SimState *state, float ax, float ay, float bx, float by) {
    if (!bee_path_hive_exists(state)) {
        return false;
    }
    const int samples = 24;
    for (int i = 1; i <= samples; ++i) {
        float t = (float)i / (float)samples;
        float px = ax + (bx - ax) * t;
        float py = ay + (by - ay) * t;
        size_t index = (size_t)SIZE_MAX;
        if (hex_world_tile_from_world(state->hex_world, px, py, &index)) {
            if (!hex_world_tile_passable(state->hex_world, index)) {
                return true;
            }
        }
    }
    return false;
}

static bool bee_path_line_clear(const SimState *state, float ax, float ay, float bx, float by, float radius) {
    if (!bee_path_point_inside_world(state, bx, by, radius)) {
        return false;
    }
    if (bee_path_segment_hits_hive(state, ax, ay, bx, by)) {
        return false;
    }
    return true;
}

bool bee_path_plan(const SimState *state,
                   size_t index,
                   float target_x,
                   float target_y,
                   float arrive_tol,
                   BeePathPlan *out_plan) {
    if (!state || index >= state->count || !out_plan) {
        return false;
    }

    BeePathPlan plan = {0};
    plan.final_x = target_x;
    plan.final_y = target_y;

    const float px = state->x[index];
    const float py = state->y[index];
    const float vx = state->vx[index];
    const float vy = state->vy[index];
    float radius = state->radius ? state->radius[index] : state->default_radius;
    if (radius <= 0.0f) {
        radius = state->default_radius > 0.0f ? state->default_radius : 1.0f;
    }

    float dx_final = target_x - px;
    float dy_final = target_y - py;
    float final_dist_sq = dx_final * dx_final + dy_final * dy_final;
    if (final_dist_sq <= arrive_tol * arrive_tol) {
        *out_plan = plan;
        return false;
    }

    bool inside_now = bee_path_point_inside_hive(state, px, py);
    bool target_inside = bee_path_point_inside_hive(state, target_x, target_y);

    if (bee_path_line_clear(state, px, py, target_x, target_y, radius)) {
        float inv_dist = 1.0f / sqrtf(final_dist_sq);
        plan.dir_x = dx_final * inv_dist;
        plan.dir_y = dy_final * inv_dist;
        plan.waypoint_x = target_x;
        plan.waypoint_y = target_y;
        plan.has_waypoint = 0;
        plan.valid = 1;
        *out_plan = plan;
        return true;
    }

    float entrance_x = target_x;
    float entrance_y = target_y;
    if (state->hex_world) {
        float center_x = entrance_x;
        float center_y = entrance_y;
        hex_world_hive_center(state->hex_world, &center_x, &center_y);
        if (!hex_world_hive_preferred_entrance(state->hex_world, &entrance_x, &entrance_y)) {
            entrance_x = center_x;
            entrance_y = center_y;
        }
    }

    float plan_target_x = target_x;
    float plan_target_y = target_y;
    uint8_t plan_uses_entrance = 0;
    if (bee_path_hive_exists(state) && inside_now != target_inside) {
        plan_target_x = entrance_x;
        plan_target_y = entrance_y;
        plan_uses_entrance = 1;
    }

    float dx = plan_target_x - px;
    float dy = plan_target_y - py;
    float dist_sq = dx * dx + dy * dy;
    if (dist_sq <= arrive_tol * arrive_tol) {
        *out_plan = plan;
        return false;
    }

    float dist = sqrtf(dist_sq);
    float inv_dist = 1.0f / dist;
    float base_dir_x = dx * inv_dist;
    float base_dir_y = dy * inv_dist;

    if (bee_path_line_clear(state, px, py, plan_target_x, plan_target_y, radius)) {
        plan.dir_x = base_dir_x;
        plan.dir_y = base_dir_y;
        plan.waypoint_x = plan_target_x;
        plan.waypoint_y = plan_target_y;
        plan.has_waypoint = plan_uses_entrance;
        plan.valid = 1;
        *out_plan = plan;
        return true;
    }

    float desired_speed = state->bee_speed_mps > 0.0f ? state->bee_speed_mps : state->max_speed;
    if (desired_speed <= 0.0f) {
        desired_speed = 100.0f;
    }
    float lookahead = dist;
    float min_ahead = radius * 6.0f;
    if (lookahead < min_ahead) {
        lookahead = min_ahead;
    }
    float max_ahead = desired_speed * 1.5f + radius * 4.0f;
    if (lookahead > max_ahead) {
        lookahead = max_ahead;
    }

    const float angles[] = {
        0.0f,
        0.35f,
        -0.35f,
        0.7f,
        -0.7f,
        1.05f,
        -1.05f,
        1.4f,
        -1.4f
    };

    float best_score = -FLT_MAX;
    float best_dir_x = base_dir_x;
    float best_dir_y = base_dir_y;
    float best_probe_x = plan_target_x;
    float best_probe_y = plan_target_y;
    bool found = false;

    float velocity_len = sqrtf(vx * vx + vy * vy);
    float vel_dir_x = 0.0f;
    float vel_dir_y = 0.0f;
    if (velocity_len > 1e-5f) {
        vel_dir_x = vx / velocity_len;
        vel_dir_y = vy / velocity_len;
    }

    for (size_t i = 0; i < (sizeof(angles) / sizeof(angles[0])); ++i) {
        float angle = angles[i];
        float cos_a = cosf(angle);
        float sin_a = sinf(angle);
        float dir_x = base_dir_x * cos_a - base_dir_y * sin_a;
        float dir_y = base_dir_x * sin_a + base_dir_y * cos_a;
        float norm = sqrtf(dir_x * dir_x + dir_y * dir_y);
        if (norm <= 1e-5f) {
            continue;
        }
        dir_x /= norm;
        dir_y /= norm;

        float probe_x = px + dir_x * lookahead;
        float probe_y = py + dir_y * lookahead;
        if (!bee_path_point_inside_world(state, probe_x, probe_y, radius)) {
            continue;
        }
        if (!bee_path_line_clear(state, px, py, probe_x, probe_y, radius)) {
            continue;
        }

        float alignment = dir_x * base_dir_x + dir_y * base_dir_y;
        float velocity_alignment = 0.0f;
        if (velocity_len > 1e-5f) {
            velocity_alignment = dir_x * vel_dir_x + dir_y * vel_dir_y;
        }
        float future_bonus = 0.0f;
        if (bee_path_line_clear(state, probe_x, probe_y, plan_target_x, plan_target_y, radius)) {
            future_bonus += 0.5f;
        }
        if (bee_path_line_clear(state, probe_x, probe_y, target_x, target_y, radius)) {
            future_bonus += 0.25f;
        }
        float angle_penalty = fabsf(angle) * 0.1f;
        float score = alignment * 1.5f + velocity_alignment * 0.6f + future_bonus - angle_penalty;
        if (score > best_score) {
            best_score = score;
            best_dir_x = dir_x;
            best_dir_y = dir_y;
            best_probe_x = probe_x;
            best_probe_y = probe_y;
            found = true;
        }
    }

    if (!found) {
        return false;
    }

    plan.dir_x = best_dir_x;
    plan.dir_y = best_dir_y;
    plan.waypoint_x = best_probe_x;
    plan.waypoint_y = best_probe_y;
    plan.has_waypoint = 1;
    plan.valid = 1;
    *out_plan = plan;
    return true;
}
