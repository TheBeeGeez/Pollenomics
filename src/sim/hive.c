#include "hive.h"

static void hive_clear_segments(SimState *state) {
    if (!state) {
        return;
    }
    state->hive_segment_count = 0;
}

static void hive_add_segment(SimState *state,
                             float ax,
                             float ay,
                             float bx,
                             float by,
                             float nx,
                             float ny) {
    if (!state) {
        return;
    }
    float dx = bx - ax;
    float dy = by - ay;
    float len_sq = dx * dx + dy * dy;
    if (len_sq < 1e-6f) {
        return;
    }
    if (state->hive_segment_count >= sizeof(state->hive_segments) / sizeof(state->hive_segments[0])) {
        return;
    }
    float n_len = sqrtf(nx * nx + ny * ny);
    if (n_len > 0.0f) {
        nx /= n_len;
        ny /= n_len;
    } else {
        nx = 0.0f;
        ny = -1.0f;
    }
    HiveSegment *seg = &state->hive_segments[state->hive_segment_count++];
    seg->ax = ax;
    seg->ay = ay;
    seg->bx = bx;
    seg->by = by;
    seg->nx = nx;
    seg->ny = ny;
}

void hive_build_segments(SimState *state) {
    hive_clear_segments(state);
    if (!state) {
        return;
    }
    if (state->hive_rect_w <= 0.0f || state->hive_rect_h <= 0.0f) {
        state->hive_enabled = 0;
        return;
    }
    state->hive_enabled = 1;

    float x = state->hive_rect_x;
    float y = state->hive_rect_y;
    float w = state->hive_rect_w;
    float h = state->hive_rect_h;
    int side = state->hive_entrance_side;

    float gap_half = state->hive_entrance_width * 0.5f;
    float gap_min = 0.0f;
    float gap_max = 0.0f;

    if (side == 0 || side == 1) {
        float side_len = w;
        float gap_center = x + state->hive_entrance_t * side_len;
        gap_min = fmaxf(x, gap_center - gap_half);
        gap_max = fminf(x + w, gap_center + gap_half);
    } else {
        float side_len = h;
        float gap_center = y + state->hive_entrance_t * side_len;
        gap_min = fmaxf(y, gap_center - gap_half);
        gap_max = fminf(y + h, gap_center + gap_half);
    }

    if (side == 0) {
        if (gap_min > x) {
            hive_add_segment(state, x, y, gap_min, y, 0.0f, -1.0f);
        }
        if (gap_max < x + w) {
            hive_add_segment(state, gap_max, y, x + w, y, 0.0f, -1.0f);
        }
    } else {
        hive_add_segment(state, x, y, x + w, y, 0.0f, -1.0f);
    }

    if (side == 1) {
        float yb = y + h;
        if (gap_min > x) {
            hive_add_segment(state, x, yb, gap_min, yb, 0.0f, 1.0f);
        }
        if (gap_max < x + w) {
            hive_add_segment(state, gap_max, yb, x + w, yb, 0.0f, 1.0f);
        }
    } else {
        hive_add_segment(state, x, y + h, x + w, y + h, 0.0f, 1.0f);
    }

    if (side == 2) {
        if (gap_min > y) {
            hive_add_segment(state, x, y, x, gap_min, -1.0f, 0.0f);
        }
        if (gap_max < y + h) {
            hive_add_segment(state, x, gap_max, x, y + h, -1.0f, 0.0f);
        }
    } else {
        hive_add_segment(state, x, y, x, y + h, -1.0f, 0.0f);
    }

    if (side == 3) {
        float xr = x + w;
        if (gap_min > y) {
            hive_add_segment(state, xr, y, xr, gap_min, 1.0f, 0.0f);
        }
        if (gap_max < y + h) {
            hive_add_segment(state, xr, gap_max, xr, y + h, 1.0f, 0.0f);
        }
    } else {
        hive_add_segment(state, x + w, y, x + w, y + h, 1.0f, 0.0f);
    }
}

static int hive_resolve_segment(const SimState *state,
                                const HiveSegment *seg,
                                float radius,
                                float *px,
                                float *py,
                                float *vx,
                                float *vy) {
    float ax = seg->ax;
    float ay = seg->ay;
    float bx = seg->bx;
    float by = seg->by;

    float abx = bx - ax;
    float aby = by - ay;
    float ab_len_sq = abx * abx + aby * aby;
    if (ab_len_sq <= 1e-8f) {
        return 0;
    }

    float apx = *px - ax;
    float apy = *py - ay;
    float t = (apx * abx + apy * aby) / ab_len_sq;
    t = clampf(t, 0.0f, 1.0f);
    float cx = ax + abx * t;
    float cy = ay + aby * t;

    float rx = *px - cx;
    float ry = *py - cy;
    float dist_sq = rx * rx + ry * ry;
    float radius_sq = radius * radius;

    if (dist_sq >= radius_sq) {
        return 0;
    }

    float dist = sqrtf(dist_sq);
    float nx;
    float ny;
    if (dist > 1e-6f) {
        nx = rx / dist;
        ny = ry / dist;
    } else {
        nx = seg->nx;
        ny = seg->ny;
        float n_len = sqrtf(nx * nx + ny * ny);
        if (n_len <= 1e-6f) {
            nx = 0.0f;
            ny = -1.0f;
        } else {
            nx /= n_len;
            ny /= n_len;
        }
    }

    float penetration = radius - dist;
    if (penetration < 0.0f) {
        return 0;
    }
    penetration += state->hive_safety_margin;
    *px += nx * penetration;
    *py += ny * penetration;

    float v_normal = (*vx) * nx + (*vy) * ny;
    float vt_x = *vx - v_normal * nx;
    float vt_y = *vy - v_normal * ny;

    float new_vn = -state->hive_restitution * v_normal;
    float new_vt_x = state->hive_tangent_damp * vt_x;
    float new_vt_y = state->hive_tangent_damp * vt_y;

    *vx = new_vn * nx + new_vt_x;
    *vy = new_vn * ny + new_vt_y;
    return 1;
}

void hive_resolve_disc(const SimState *state,
                       float radius,
                       float *x,
                       float *y,
                       float *vx,
                       float *vy) {
    if (!state || !state->hive_enabled || state->hive_segment_count == 0) {
        return;
    }
    int max_iters = state->hive_max_iters > 0 ? state->hive_max_iters : 1;
    for (int iter = 0; iter < max_iters; ++iter) {
        int collided = 0;
        for (size_t si = 0; si < state->hive_segment_count; ++si) {
            collided |= hive_resolve_segment(state, &state->hive_segments[si], radius, x, y, vx, vy);
        }
        if (!collided) {
            break;
        }
    }
}

void hive_compute_points(const SimState *state,
                         float *entrance_x,
                         float *entrance_y,
                         float *unload_x,
                         float *unload_y) {
    if (!state) {
        if (entrance_x) *entrance_x = 0.0f;
        if (entrance_y) *entrance_y = 0.0f;
        if (unload_x) *unload_x = 0.0f;
        if (unload_y) *unload_y = 0.0f;
        return;
    }
    const float hx = state->hive_rect_x;
    const float hy = state->hive_rect_y;
    const float hw = state->hive_rect_w;
    const float hh = state->hive_rect_h;
    const float t = clampf(state->hive_entrance_t, 0.0f, 1.0f);
    float ex = hx + hw * 0.5f;
    float ey = hy + hh * 0.5f;
    switch (state->hive_entrance_side) {
        case 0:
            ex = hx + hw * t;
            ey = hy;
            break;
        case 1:
            ex = hx + hw * t;
            ey = hy + hh;
            break;
        case 2:
            ex = hx;
            ey = hy + hh * t;
            break;
        case 3:
            ex = hx + hw;
            ey = hy + hh * t;
            break;
        default:
            break;
    }
    if (entrance_x) *entrance_x = ex;
    if (entrance_y) *entrance_y = ey;

    float unload_px = hx + hw * 0.5f;
    float unload_py = hy + hh * 0.6f;
    if (unload_x) *unload_x = unload_px;
    if (unload_y) *unload_y = unload_py;
}
