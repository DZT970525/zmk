/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct zitaotech_trackpad_curve {
    bool enabled;
    uint32_t minimum;
    uint32_t maximum;
    uint32_t exponent;
    uint32_t vmax;
    uint32_t ramp_ms;
    uint32_t ramp_counts;
    uint32_t idle_ms;
    uint32_t smoothing;
};
#if defined(CONFIG_BOARD_BBP9981)
#define ZITAOTECH_TRACKPAD_CURVE_DEFAULT_ENABLED false
#else
#define ZITAOTECH_TRACKPAD_CURVE_DEFAULT_ENABLED true
#endif
#if defined(CONFIG_BOARD_ZITAOTECH_Q10) || defined(CONFIG_BOARD_BBP9981) || defined(CONFIG_BOARD_ZITAOTECH_Q20)
#define ZITAOTECH_TRACKPAD_CURVE_DEFAULT \
    ((struct zitaotech_trackpad_curve){ZITAOTECH_TRACKPAD_CURVE_DEFAULT_ENABLED, 66, 135, 148, 600, 419, 853, 60, 50})
#else
#define ZITAOTECH_TRACKPAD_CURVE_DEFAULT \
    ((struct zitaotech_trackpad_curve){ZITAOTECH_TRACKPAD_CURVE_DEFAULT_ENABLED, 56, 170, 127, 450, 419, 853, 60, 50})
#endif
static inline bool zitaotech_trackpad_curve_valid(const struct zitaotech_trackpad_curve *c) {
    return c->minimum >= 10 && c->maximum <= 500 && c->minimum <= c->maximum &&
           c->exponent >= 10 && c->exponent <= 400 && c->vmax >= 10 && c->vmax <= 2000 &&
           c->ramp_ms >= 10 && c->ramp_ms <= 3000 && c->ramp_counts >= 1 &&
           c->ramp_counts <= 2000 && c->idle_ms >= 10 && c->idle_ms <= 1000 &&
           c->smoothing >= 1 && c->smoothing <= 100;
}
/* Called after sensor detection; saved/user values always take precedence. */
void zitaotech_trackpad_curve_set_device_default(bool enabled);
bool zitaotech_trackpad_curve_supported(void);
void zitaotech_trackpad_curve_get(struct zitaotech_trackpad_curve *out);
int zitaotech_trackpad_curve_set(const struct zitaotech_trackpad_curve *value);
void zitaotech_trackpad_curve_reset(void);
