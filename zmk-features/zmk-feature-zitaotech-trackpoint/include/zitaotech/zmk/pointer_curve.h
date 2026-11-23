/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* All values use hundredths: counts/ms, exponent, and gain respectively. */
struct zitaotech_pointer_curve {
    uint16_t vmax;
    uint16_t exponent;
    uint16_t minimum;
    uint16_t maximum;
    bool enabled;
};
#define ZITAOTECH_POINTER_CURVE_DEFAULT \
    ((struct zitaotech_pointer_curve){450, 90, 62, 170, true})

static inline bool zitaotech_pointer_curve_valid(const struct zitaotech_pointer_curve *c) {
    return c->vmax >= 10 && c->vmax <= 2000 && c->exponent >= 10 && c->exponent <= 400 &&
           c->minimum >= 10 && c->minimum <= 300 && c->maximum >= 10 && c->maximum <= 500 &&
           c->minimum <= c->maximum;
}
void zitaotech_pointer_curve_get(struct zitaotech_pointer_curve *out);
int zitaotech_pointer_curve_set(const struct zitaotech_pointer_curve *value);
void zitaotech_pointer_curve_reset(void);
void zitaotech_pointer_curve_save(void);
#define ZITAOTECH_POINTER_RELEASE_DECAY_DEFAULT 80
#define ZITAOTECH_POINTER_RISE_RATE_DEFAULT 35
uint8_t zitaotech_pointer_release_decay_get(void);
int zitaotech_pointer_release_decay_set(uint8_t value);
uint8_t zitaotech_pointer_rise_rate_get(void);
int zitaotech_pointer_rise_rate_set(uint8_t value);
