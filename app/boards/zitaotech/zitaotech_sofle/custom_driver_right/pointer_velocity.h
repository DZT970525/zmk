/* SPDX-License-Identifier: MIT
 * ca36c8e calibrated velocity curve, evaluated on paired raw I2C samples.
 * Runtime DPI, burst and directional trim are applied downstream unchanged.
 */
#ifndef KEYPOINT_POINTER_VELOCITY_H
#define KEYPOINT_POINTER_VELOCITY_H
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <zitaotech/zmk/pointer_curve.h>

struct keypoint_pointer_velocity {
    float speed_ema;
    float remainder_x;
    float remainder_y;
    uint32_t last_ms;
    bool active;
    struct zitaotech_pointer_curve config;
};

static inline void keypoint_pointer_velocity_reset(struct keypoint_pointer_velocity *s) {
    *s = (struct keypoint_pointer_velocity){0};
}

static inline void keypoint_pointer_velocity_apply(struct keypoint_pointer_velocity *s,
                                                  int32_t dx, int32_t dy, uint32_t now,
                                                  float mode_scale, int32_t *x, int32_t *y) {
    struct zitaotech_pointer_curve c;
    zitaotech_pointer_curve_get(&c);
    if (s->config.enabled != c.enabled || s->config.vmax != c.vmax ||
        s->config.exponent != c.exponent || s->config.minimum != c.minimum ||
        s->config.maximum != c.maximum) {
        keypoint_pointer_velocity_reset(s);
        s->config = c;
    }
    uint32_t dt = now - s->last_ms;
    if (!s->active || dt > 60) {
        s->speed_ema = 0.0f;
    } else {
        if (dt == 0) dt = 1;
        float speed = hypotf((float)dx, (float)dy) / (float)dt;
        float alpha = speed < s->speed_ema
            ? (float)zitaotech_pointer_release_decay_get() / 100.0f
            : (float)zitaotech_pointer_rise_rate_get() / 100.0f;
        s->speed_ema += alpha * (speed - s->speed_ema);
    }
    s->active = true;
    s->last_ms = now;
    float t = fminf(s->speed_ema / ((float)c.vmax / 100.0f), 1.0f);
    float curve_gain = c.enabled
        ? ((float)c.minimum + (float)(c.maximum - c.minimum) * powf(t, (float)c.exponent / 100.0f)) / 100.0f
        : 1.0f;
    float gain = 0.5f * curve_gain * mode_scale;
    s->remainder_x += (float)dx * gain;
    s->remainder_y += (float)dy * gain;
    *x = (int32_t)s->remainder_x;
    *y = (int32_t)s->remainder_y;
    s->remainder_x -= *x;
    s->remainder_y -= *y;
}
#endif
