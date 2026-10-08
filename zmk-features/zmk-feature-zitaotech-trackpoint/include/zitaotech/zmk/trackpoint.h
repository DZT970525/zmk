/*
 * Copyright (c) 2026 ZitaoTech
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/event_manager.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TrackPoint pointer speed is expressed as a DPI value; the central scales the
 * motion it receives over the split link by a factor derived from it. The UI
 * exposes it as ten discrete steps, the same range the &DPI keys have always
 * used.
 */
#define ZITAOTECH_TRACKPOINT_DPI_LEVELS                                                            \
    ((CONFIG_ZITAOTECH_TRACKPOINT_DPI_MAX - CONFIG_ZITAOTECH_TRACKPOINT_DPI_MIN) /                \
         CONFIG_ZITAOTECH_TRACKPOINT_DPI_STEP +                                                    \
     1)

/** Current pointer speed as a DPI value (e.g. 40). */
uint32_t zitaotech_trackpoint_dpi_value(void);

/** Current pointer speed as a zero-based step index. */
uint32_t zitaotech_trackpoint_dpi_index(void);

/** Set the pointer speed by step index. Returns the new index or a negative errno. */
int zitaotech_trackpoint_set_dpi_index(uint32_t index);

/** Move the pointer speed by +1/-1 step, clamped. Returns the new index. */
int zitaotech_trackpoint_step_dpi(int delta);

/**
 * Burst mode multiplies the pointer speed on top of the DPI step (x1..x10), for
 * quickly crossing a screen without leaving the comfortable DPI setting.
 */
bool zitaotech_trackpoint_burst_enabled(void);
uint32_t zitaotech_trackpoint_burst_multiplier(void);

/** Effective pointer multiplier: the burst multiplier when burst is on, else 1. */
uint32_t zitaotech_trackpoint_pointer_multiplier(void);

/** Set burst mode on/off and its multiplier (1..CONFIG_ZITAOTECH_TRACKPOINT_BURST_MAX). */
int zitaotech_trackpoint_set_burst(bool enabled, uint32_t multiplier);

/*
 * Per-direction fine tuning.
 *
 * A TrackPoint is rarely equally sensitive in all four directions, so each one
 * gets its own multiplier on top of the DPI step.  Values are hundredths
 * (100 = x1.00) and are clamped to the Kconfig range.
 */
enum zitaotech_trackpoint_axis {
    ZITAOTECH_TRACKPOINT_AXIS_X_POSITIVE = 0,
    ZITAOTECH_TRACKPOINT_AXIS_X_NEGATIVE = 1,
    ZITAOTECH_TRACKPOINT_AXIS_Y_POSITIVE = 2,
    ZITAOTECH_TRACKPOINT_AXIS_Y_NEGATIVE = 3,
    ZITAOTECH_TRACKPOINT_AXIS_COUNT = 4,
};

/** One axis's multiplier in hundredths (100 = x1.00). */
uint32_t zitaotech_trackpoint_axis_scale(enum zitaotech_trackpoint_axis axis);

/** Set all four multipliers at once, in hundredths. Returns 0 or a negative errno. */
int zitaotech_trackpoint_set_axis_scales(const uint32_t scales[ZITAOTECH_TRACKPOINT_AXIS_COUNT]);

/** Put every axis back to x1.00. */
int zitaotech_trackpoint_reset_axis_scales(void);

/*
 * Linear acceleration.
 *
 * While the TrackPoint keeps moving the same way, the pointer speed ramps up
 * linearly from x1.00 to this multiplier; letting go or reversing drops it back
 * to x1.00.  The value is hundredths (100 = x1.00, i.e. off) and applies to
 * every direction.
 */
uint32_t zitaotech_trackpoint_accel(void);
int zitaotech_trackpoint_set_accel(uint32_t accel);

/*
 * Scrolling.  The TrackPoint scrolls while its scroll key is held; these scale
 * the wheel steps it emits, on top of the driver's own threshold.
 */
uint32_t zitaotech_trackpoint_scroll_speed_percent(void);
bool zitaotech_trackpoint_scroll_burst_enabled(void);
uint32_t zitaotech_trackpoint_scroll_burst_multiplier(void);

/** Effective scroll multiplier: speed percent times the burst multiplier. */
uint32_t zitaotech_trackpoint_scroll_multiplier(void);

/**
 * Scroll acceleration ceiling, in hundredths (100 = x1.00 = off).  While the
 * scroll key is held and the wheel keeps turning the same way, the scroll speed
 * ramps up to this; the ramp is longer than the pointer's (see
 * CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_ACCEL_RAMP_MS).
 */
uint32_t zitaotech_trackpoint_scroll_accel(void);

/**
 * Keymap position that puts the TrackPoint into scroll mode while held
 * (-1 = unassigned).
 */
/** Convenience wrappers for the scroll mode key (ZITAOTECH_TRACKPOINT_MODE_SCROLL). */
int32_t zitaotech_trackpoint_scroll_key_position(void);
int zitaotech_trackpoint_set_scroll_key_position(int32_t position);
bool zitaotech_trackpoint_scroll_key_pressed(void);

/** Caps Lock also puts the TrackPoint into scroll mode (on by default). */
bool zitaotech_trackpoint_scroll_capslock_enabled(void);
bool zitaotech_trackpoint_smooth_scroll_enabled(void);
int zitaotech_trackpoint_set_smooth_scroll(bool enabled);
/** Scroll direction flips, applied on top of the driver's compile-time signs. */
bool zitaotech_trackpoint_scroll_invert_x(void);
bool zitaotech_trackpoint_scroll_invert_y(void);
int zitaotech_trackpoint_set_scroll_options(bool capslock_scroll, bool invert_x, bool invert_y);

/** Returns 0, or -ERANGE when a value is outside the Kconfig range. */
int zitaotech_trackpoint_set_scroll(uint32_t speed_percent, bool burst_enabled,
                                    uint32_t burst_multiplier, uint32_t accel);

/** Restore every value to its Kconfig default and persist that. */
int zitaotech_trackpoint_reset(void);

/** Flush pending values to flash. */
int zitaotech_trackpoint_save(void);

/**
 * Hook called after any value changes. The Studio handler overrides it to push
 * a notification; it is a no-op when the studio RPC is not compiled in.
 */
void zitaotech_trackpoint_state_changed(void);

/**
 * Suppress the change notification while a Studio request is being applied: the
 * RPC response already carries the new state.
 */
void zitaotech_trackpoint_suppress_notifications(bool suppress);

/**
 * Raised on every DPI change, on both halves. The board's TrackPoint LED
 * listens for it to light up at a brightness derived from the new DPI.
 */
struct zitaotech_trackpoint_dpi_changed {
    uint32_t dpi_value;
    uint32_t dpi_index;
};

ZMK_EVENT_DECLARE(zitaotech_trackpoint_dpi_changed);

/**
 * Mirrored state, relayed between the halves so both agree:
 *
 *  - the central scales the TrackPoint motion with its own copy, and
 *  - the peripheral drives the TrackPoint LED from its own copy.
 *
 * Either half can be the one that changes the value (the &DPI keys run where
 * the key physically sits, Studio talks to the central), so the event is
 * relayed in both directions.
 */
struct zitaotech_trackpoint_mirror {
    uint8_t dpi_index;
    uint8_t burst_enabled;
    uint8_t burst_multiplier;
    /* uint16 so the whole range Studio offers fits; applies on the half that
     * actually reads the TrackPoint (the peripheral). */
    uint8_t smooth_scroll_enabled;
    uint16_t arrow_threshold;
    uint8_t capslock_scroll_enabled;
    uint8_t source;
};

ZMK_EVENT_DECLARE(zitaotech_trackpoint_mirror);

/*
 * The four "mode keys": keymap positions that change how the TrackPoint behaves
 * while held.  They are user-assignable from ZitaoTech Studio (-1 = unassigned)
 * and may sit on either half, so the central relays their state to the
 * peripheral (which is where the TrackPoint actually is).
 */
enum zitaotech_trackpoint_mode {
    ZITAOTECH_TRACKPOINT_MODE_ARROW = 0,
    ZITAOTECH_TRACKPOINT_MODE_FAST = 1,
    ZITAOTECH_TRACKPOINT_MODE_SLOW = 2,
    ZITAOTECH_TRACKPOINT_MODE_SCROLL = 3,
    ZITAOTECH_TRACKPOINT_MODE_COUNT = 4,
};

int32_t zitaotech_trackpoint_mode_key_position(enum zitaotech_trackpoint_mode mode);
int zitaotech_trackpoint_set_mode_key(enum zitaotech_trackpoint_mode mode, int32_t position);
bool zitaotech_trackpoint_mode_key_pressed(enum zitaotech_trackpoint_mode mode);

/** Arrow mode: raw travel before an arrow key is emitted (lower = twitchier). */
uint32_t zitaotech_trackpoint_arrow_swipe_threshold(void);
int zitaotech_trackpoint_set_arrow_swipe_threshold(uint32_t threshold);

/** Mode key bitmap (one bit per mode), relayed from the central. */
struct zitaotech_trackpoint_scroll_key {
    uint8_t mask;
    uint8_t source;
};

ZMK_EVENT_DECLARE(zitaotech_trackpoint_scroll_key);

#ifdef __cplusplus
}
#endif
