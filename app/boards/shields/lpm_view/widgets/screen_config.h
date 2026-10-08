/*
 * Copyright (c) 2026 ZitaoTech
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/event_manager.h>

/*
 * Picture-carousel configuration for the right half's screen.
 *
 * The values live in the central half's custom settings, so ZitaoTech Studio
 * can read and write them through the generic custom-settings RPC and they
 * survive a reboot.  The carousel itself runs on the peripheral half, which
 * receives a small mirrored copy over the split relay - the relay payload has
 * to fit one ATT write, so it carries only the interval and the picture mask.
 */

/** Pictures in the rotation.  Bit i of an enabled mask is picture i. */
#define ZITAOTECH_SCREEN_PICTURES 10

#define ZITAOTECH_SCREEN_INTERVAL_DEFAULT_S 60
#define ZITAOTECH_SCREEN_INTERVAL_MIN_S 5
#define ZITAOTECH_SCREEN_INTERVAL_MAX_S 600
#define ZITAOTECH_SCREEN_PICTURES_DEFAULT 0x3ff

/** Seconds each picture stays on screen. */
uint32_t zitaotech_screen_interval_s(void);

/** Bit i set = picture i takes part in the rotation (bit 0 = cat). */
uint16_t zitaotech_screen_enabled_mask(void);

/**
 * Apply a new configuration.  The values are clamped, persisted on this half
 * and mirrored to the other one, and a change is announced through
 * zitaotech_screen_state_changed().
 */
int zitaotech_screen_set_interval_s(uint32_t seconds);
int zitaotech_screen_set_enabled_mask(uint16_t mask);
int zitaotech_screen_reset(void);

/** What the right half last reported showing (central half's view). */
bool zitaotech_screen_right_connected(void);
uint32_t zitaotech_screen_right_interval_s(void);
uint16_t zitaotech_screen_right_applied_mask(void);

/** Weak; the Studio RPC handler overrides it to push a notification. */
void zitaotech_screen_state_changed(void);

/** The mirrored configuration; only the peripheral's copy drives a screen. */
struct zitaotech_screen_config_mirror_v3 {
    uint16_t interval_ticks;
    uint8_t order[5];
    uint16_t enabled_mask;
    uint8_t source;
};

/**
 * Raised by the peripheral to ask the central for the stored configuration,
 * so a half that has just powered up (or been reflashed) does not keep showing
 * the factory defaults until the next change.
 */
struct zitaotech_screen_config_request_v3 {
    uint8_t source;
};

/**
 * Raised by the peripheral after it applied a configuration, so the central
 * (and through it, Studio) can tell that the right half really took the values
 * instead of only being asked to.
 */
struct zitaotech_screen_config_applied_v3 {
    uint16_t interval_ticks;
    uint8_t order[5];
    uint16_t enabled_mask;
    uint8_t source;
};

/** The central asks the peripheral what it is showing (answered with the
 * event above), so a report is fresh by the time Studio reads the state. */
struct zitaotech_screen_config_query_v3 {
    uint8_t source;
};

ZMK_EVENT_DECLARE(zitaotech_screen_config_mirror_v3);
ZMK_EVENT_DECLARE(zitaotech_screen_config_request_v3);
ZMK_EVENT_DECLARE(zitaotech_screen_config_applied_v3);
ZMK_EVENT_DECLARE(zitaotech_screen_config_query_v3);

/** Ask the right half to report; returns immediately. */
void zitaotech_screen_refresh_right(void);

/**
 * Implemented by the picture carousel widget: re-read the configuration after
 * it changed (the interval, or which pictures are enabled).
 */
void zmk_widget_status_carousel_config_changed(void);

#define ZITAOTECH_SCREEN_INTERVAL_MIN_MS 100
#define ZITAOTECH_SCREEN_INTERVAL_MAX_MS 600000
uint32_t zitaotech_screen_interval_ms(void);
bool zitaotech_screen_sequential(void);
void zitaotech_screen_playback_order(uint8_t out[5]);
uint32_t zitaotech_screen_right_interval_ms(void);
bool zitaotech_screen_right_sequential(void);
void zitaotech_screen_right_playback_order(uint8_t out[5]);
int zitaotech_screen_set_playback(uint32_t ms, bool sequential, const uint8_t order[5]);


