/* SPDX-License-Identifier: MIT
 * Paired X/Y velocity curve adapted from Sofle's pointer_velocity.h.
 * Gain is independent of Studio DPI; the existing DPI/fast/slow scale follows it.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <zitaotech/zmk/trackpad_curve.h>

struct bbp_pointer_accel {
    uint32_t last_ms;
    uint32_t elapsed_ms;
    float travel;
    float speed_ema;
    int16_t last_x;
    int16_t last_y;
    bool active;
    struct zitaotech_trackpad_curve config;
};

static inline void bbp_pointer_accel_reset(struct bbp_pointer_accel *s) {
    *s = (struct bbp_pointer_accel){0};
}

static inline float bbp_pointer_accel_gain_config(struct bbp_pointer_accel *s,
        int16_t dx, int16_t dy, uint32_t now, const struct zitaotech_trackpad_curve *c) {
    const struct zitaotech_trackpad_curve *old = &s->config;
    if (old->enabled != c->enabled || old->minimum != c->minimum || old->maximum != c->maximum ||
        old->exponent != c->exponent || old->vmax != c->vmax || old->ramp_ms != c->ramp_ms ||
        old->ramp_counts != c->ramp_counts || old->idle_ms != c->idle_ms || old->smoothing != c->smoothing) {
        bbp_pointer_accel_reset(s);
        s->config = *c;
    }
    if (!c->enabled) return 1.0f;
    /* Zero packets must not extend a gesture or increase its gain. */
    if (!dx && !dy) return c->minimum / 100.0f;
    uint32_t dt = now - s->last_ms;
    int32_t dot = (int32_t)dx * s->last_x + (int32_t)dy * s->last_y;
    if (!s->active || dt > c->idle_ms || dot <= 0) {
        bbp_pointer_accel_reset(s);
        s->config = *c;
    } else {
        if (!dt) dt = 1;
        float distance = hypotf((float)dx, (float)dy);
        s->speed_ema += (c->smoothing / 100.0f) * (distance / dt - s->speed_ema);
        s->elapsed_ms = s->elapsed_ms + dt > c->ramp_ms ? c->ramp_ms : s->elapsed_ms + dt;
        s->travel = fminf(s->travel + distance, (float)c->ramp_counts);
    }
    s->active = true;
    s->last_ms = now;
    s->last_x = dx;
    s->last_y = dy;
    /* Tiny back-and-forth adjustments stay slow; a sustained directional
     * stroke ramps by elapsed time AND distance, not by packet count. */
    float sustained = fminf((float)s->elapsed_ms / c->ramp_ms, s->travel / c->ramp_counts);
    float velocity = s->speed_ema / (c->vmax / 100.0f);
    float t = fminf(fmaxf(velocity, sustained), 1.0f);
    return (c->minimum + (c->maximum - c->minimum) * powf(t, c->exponent / 100.0f)) / 100.0f;
}

static inline float bbp_pointer_accel_gain(struct bbp_pointer_accel *s,
                                         int16_t dx, int16_t dy, uint32_t now) {
    const struct zitaotech_trackpad_curve c = ZITAOTECH_TRACKPAD_CURVE_DEFAULT;
    return bbp_pointer_accel_gain_config(s, dx, dy, now, &c);
}
