/*
 * Runtime backlight control for ZitaoTech boards.
 *
 * Copyright (c) 2026 ZitaoTech
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Effect the layer indicator LED shows while a layer is active. Layer 0 is not
 * configurable and always behaves like ZITAOTECH_BACKLIGHT_EFFECT_BACKLIGHT;
 * the automatic mouse layer is treated as layer 0 as well.
 */
enum zitaotech_backlight_effect {
    ZITAOTECH_BACKLIGHT_EFFECT_BLINK = 0,
    ZITAOTECH_BACKLIGHT_EFFECT_BREATH = 1,
    ZITAOTECH_BACKLIGHT_EFFECT_FAST_BLINK = 2,
    ZITAOTECH_BACKLIGHT_EFFECT_OFF = 3,
    ZITAOTECH_BACKLIGHT_EFFECT_BACKLIGHT = 4,
    ZITAOTECH_BACKLIGHT_EFFECT_COUNT,
};

/** Effect configured for a layer (1..CONFIG_ZITAOTECH_BACKLIGHT_LAYER_COUNT). */
uint8_t zitaotech_backlight_layer_effect(uint32_t layer);

/** Change one layer's effect. Returns 0, -EINVAL or -ENOTSUP. */
int zitaotech_backlight_set_layer_effect(uint32_t layer, uint8_t effect);

/** Number of configurable layers (layer 1 .. this value). */
uint32_t zitaotech_backlight_layer_count(void);

/*
 * Backlight on/off and brightness. These route straight through to ZMK's own
 * backlight, which already persists its state.
 */
bool zitaotech_backlight_is_on(void);
uint8_t zitaotech_backlight_brightness(void);
int zitaotech_backlight_set_on(bool on);
int zitaotech_backlight_set_brightness(uint8_t brightness);

/** Brightness limits the UI may select. */
uint8_t zitaotech_backlight_brightness_min(void);
uint8_t zitaotech_backlight_brightness_max(void);

/** Hook the Studio handler overrides to push a notification. */
void zitaotech_backlight_state_changed(void);

/** Suppress the notification while a Studio request is being applied. */
void zitaotech_backlight_suppress_notifications(bool suppress);

#ifdef __cplusplus
}
#endif
