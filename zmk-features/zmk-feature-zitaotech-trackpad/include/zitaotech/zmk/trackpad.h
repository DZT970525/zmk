/*
 * Copyright (c) 2026 ZitaoTech
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/hid_indicators.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Which endpoint profile carries the Caps Lock indicator.
 *
 * ZMK keeps one indicator profile per endpoint and "the current profile" is
 * the one belonging to the *selected* endpoint.  On this wired-only board the
 * selection falls back to the NONE slot whenever the USB bus is reset or
 * re-enumerated, and that slot always reads as zero.  Everything that derives
 * from Caps Lock - the trackpad LED animation and the Caps Lock scroll mode -
 * therefore switched off on its own, and the next Caps Lock press, which
 * toggles whatever "current" happens to be, brought the indication back
 * inverted.
 *
 * The host's LED reports (control SET_REPORT and the interrupt OUT endpoint
 * used by some hosts) always land in the USB profile, so a wired build pins the
 * Caps Lock state to the USB profile and stops caring about the selected
 * endpoint.  A wireless build still follows the selected endpoint, i.e. the
 * BLE profile, exactly as before.
 */
static inline struct zmk_endpoint_instance zitaotech_caps_endpoint(void) {
#if IS_ENABLED(CONFIG_ZMK_BLE)
    return zmk_endpoint_get_selected();
#else
    return (struct zmk_endpoint_instance){.transport = ZMK_TRANSPORT_USB};
#endif
}

/** HID indicators (Caps Lock, Num Lock, ...) of the profile used for Caps Lock. */
static inline zmk_hid_indicators_t zitaotech_caps_indicators(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    return zmk_hid_indicators_get_current_profile();
#else
    return zmk_hid_indicators_get_profile(zitaotech_caps_endpoint());
#endif
}

/*
 * A320 pointer speed is expressed as a DPI value; the board driver multiplies
 * each raw sample by a factor derived from it. The UI exposes it as ten
 * discrete steps (the hardware/curve range the driver has always used).
 */
#define ZITAOTECH_TRACKPAD_DPI_LEVELS                                                             \
    ((CONFIG_ZITAOTECH_TRACKPAD_DPI_MAX - CONFIG_ZITAOTECH_TRACKPAD_DPI_MIN) /                    \
         CONFIG_ZITAOTECH_TRACKPAD_DPI_STEP +                                                      \
     1)

/** Current pointer speed as a DPI value (e.g. 40). */
uint32_t zitaotech_trackpad_dpi_value(void);

/** Current pointer speed as a zero-based step index (0..ZITAOTECH_TRACKPAD_DPI_LEVELS-1). */
uint32_t zitaotech_trackpad_dpi_index(void);

/** Set the pointer speed by step index. Returns 0, -EINVAL or -ERANGE. */
int zitaotech_trackpad_set_dpi_index(uint32_t index);

/** Move the pointer speed by +1/-1 step, clamped. Returns the new index. */
int zitaotech_trackpad_step_dpi(int delta);

/**
 * Burst mode multiplies the pointer speed on top of the DPI step (x1..x10),
 * for quickly crossing a screen without leaving the comfortable DPI setting.
 */
bool zitaotech_trackpad_burst_enabled(void);
uint32_t zitaotech_trackpad_burst_multiplier(void);

/**
 * Effective pointer multiplier: the configured burst multiplier when burst
 * mode is on, otherwise 1.
 */
uint32_t zitaotech_trackpad_pointer_multiplier(void);

/** Set burst mode on/off and its multiplier (1..CONFIG_ZITAOTECH_TRACKPAD_BURST_MAX). */
int zitaotech_trackpad_set_burst(bool enabled, uint32_t multiplier);

/**
 * Smooth (high-resolution) scrolling. When on, ZMK keeps the HID resolution
 * multiplier path enabled so hosts that support it scroll in finer steps.
 */
bool zitaotech_trackpad_smooth_scroll_enabled(void);
int zitaotech_trackpad_set_smooth_scroll(bool enabled);

/**
 * Whether the host's Caps Lock indicator turns the trackpad into scroll mode,
 * which is what the stock Q10 firmware did.  On by default; ZitaoTech Studio
 * can turn it off for users who would rather have Caps Lock do nothing else.
 */
bool zitaotech_trackpad_caps_lock_scroll_enabled(void);
int zitaotech_trackpad_set_caps_lock_scroll(bool enabled);

/**
 * Automatic mouse layer.
 *
 * While a finger is on the trackpad the firmware can hold
 * CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER active, which the board keymap defines
 * as layer 0 plus two overrides: Q becomes the left mouse button and the
 * rightmost key of the top row the right mouse button. Holding a key needs
 * less force than pressing the trackpad, so dragging is smoother with it.
 *
 * Off by default; ZitaoTech Studio switches it on per keyboard.
 */
bool zitaotech_trackpad_mouse_layer_enabled(void);
int zitaotech_trackpad_set_mouse_layer(bool enabled);

/** How long the mouse layer stays up after the last trackpad movement (ms). */
uint32_t zitaotech_trackpad_mouse_layer_ms(void);
int zitaotech_trackpad_set_mouse_layer_ms(uint32_t ms);

/**
 * The TrackPoint half has its own switch and duration for the same layer: its
 * motion reaches this (the central) half over the split link, but the layer
 * itself lives here.
 */
bool zitaotech_trackpad_trackpoint_mouse_layer_enabled(void);
int zitaotech_trackpad_set_trackpoint_mouse_layer(bool enabled);
uint32_t zitaotech_trackpad_trackpoint_mouse_layer_ms(void);
int zitaotech_trackpad_set_trackpoint_mouse_layer_ms(uint32_t ms);

/**
 * Which pointing device is currently holding the mouse layer up.  The trackpad
 * half of the SOFLE drives its own LED from this: it lights up only when the
 * layer came from the pad, not when the TrackPoint brought it up.
 */
bool zitaotech_trackpad_mouse_layer_from_trackpad(void);

/** Raised when that changes while the layer is up. */
struct zitaotech_trackpad_mouse_layer_source_changed {
    bool from_trackpad;
};

ZMK_EVENT_DECLARE(zitaotech_trackpad_mouse_layer_source_changed);

/**
 * How long the trackpad counts as "touched" after the last movement, in
 * milliseconds. Drives both the trackpad LED timeout and how long the
 * automatic mouse layer stays active after the finger leaves the pad.
 */
uint32_t zitaotech_trackpad_touch_on_ms(void);
int zitaotech_trackpad_set_touch_on_ms(uint32_t ms);

/** Scroll burst: x1..x5 on top of the normal scroll speed. */
bool zitaotech_trackpad_scroll_burst_enabled(void);
uint32_t zitaotech_trackpad_scroll_burst_multiplier(void);
uint32_t zitaotech_trackpad_scroll_multiplier(void);
int zitaotech_trackpad_set_scroll_burst(bool enabled, uint32_t multiplier);

/**
 * Direction-mode swipe threshold: how far a finger travels before an arrow key
 * is emitted. Lower = more sensitive.
 */
uint32_t zitaotech_trackpad_arrow_swipe_threshold(void);
int zitaotech_trackpad_set_arrow_swipe_threshold(uint32_t threshold);

/**
 * Which mode the trackpad uses when no mode key is held.
 *
 * The direction/pointer mode key (see ZITAOTECH_TRACKPAD_KEY_ARROW) switches to
 * the other mode for as long as it is held, so a pointer-mode keyboard gets a
 * "direction mode" key and a direction-mode keyboard gets a "pointer mode"
 * one - the same physical key either way.
 */
enum zitaotech_trackpad_mode {
    ZITAOTECH_TRACKPAD_MODE_POINTER = 0,
    ZITAOTECH_TRACKPAD_MODE_ARROW = 1,
};

uint32_t zitaotech_trackpad_mode(void);
int zitaotech_trackpad_set_mode(uint32_t mode);

/**
 * Whether the mode-switch key (&TP) may toggle between pointer and direction
 * mode.
 *
 * Off until ZitaoTech Studio turns it on: the key stays assigned in the keymap
 * but does nothing, so an accidental assignment cannot change the mode.
 */
bool zitaotech_trackpad_mode_switch_enabled(void);
int zitaotech_trackpad_set_mode_switch_enabled(bool enabled);

/**
 * Toggle the base mode.
 *
 * Returns the new mode, or -EACCES while the mode-switch key is disabled.
 */
int zitaotech_trackpad_toggle_mode(void);

/*
 * Mode keys: keymap positions that switch how the trackpad behaves while
 * they are held. -1 means "no key assigned" and disables that mode.
 */
enum zitaotech_trackpad_mode_key {
    ZITAOTECH_TRACKPAD_KEY_ARROW = 0,
    ZITAOTECH_TRACKPAD_KEY_FAST = 1,
    ZITAOTECH_TRACKPAD_KEY_SLOW = 2,
    ZITAOTECH_TRACKPAD_KEY_SCROLL = 3,
    ZITAOTECH_TRACKPAD_KEY_FAST_SCROLL = 4,
    ZITAOTECH_TRACKPAD_KEY_COUNT = 5,
};

int32_t zitaotech_trackpad_arrow_key_position(void);
int32_t zitaotech_trackpad_fast_key_position(void);
int32_t zitaotech_trackpad_slow_key_position(void);
int32_t zitaotech_trackpad_fast_scroll_key_position(void);
int32_t zitaotech_trackpad_scroll_key_position(void);

/** Assign one mode key; pass position < 0 to clear it. */
int zitaotech_trackpad_set_mode_key(uint8_t mode, int32_t position);

/**
 * Assign all mode keys at once. Every position must be -1 (unassigned) or
 * unique; duplicates are rejected with -EINVAL so one key can never drive two
 * modes. Applying them together is what makes swapping two assignments work.
 */
int zitaotech_trackpad_set_mode_keys(const int32_t positions[ZITAOTECH_TRACKPAD_KEY_COUNT]);

/** Scroll speed multiplier in percent. */
uint32_t zitaotech_trackpad_scroll_speed_percent(void);
int zitaotech_trackpad_set_scroll_speed_percent(uint32_t percent);

/** Scroll axis inversion, applied on top of the board's compile-time signs. */
bool zitaotech_trackpad_scroll_invert_x(void);
bool zitaotech_trackpad_scroll_invert_y(void);
int zitaotech_trackpad_set_scroll_invert(bool invert_x, bool invert_y);

/** Restore every value to its Kconfig default and persist that. */
int zitaotech_trackpad_reset(void);

/** Flush pending values to flash. */
int zitaotech_trackpad_save(void);

/**
 * Hook called after any value changes. The Studio handler overrides it to push
 * a notification; it is a no-op when the studio RPC is not compiled in.
 */
void zitaotech_trackpad_state_changed(void);

/**
 * Suppress the change notification while a Studio request is being applied:
 * the RPC response already carries the new state, so notifying as well would
 * duplicate it (and interleave with the response frame).
 */
void zitaotech_trackpad_suppress_notifications(bool suppress);

/**
 * Raised on every DPI change (keyboard keys or Studio). The board's trackpad
 * LED listens for it to flash the trackpad backlight at a brightness derived
 * from the new DPI.
 */
struct zitaotech_trackpad_dpi_changed {
    uint32_t dpi_value;
    uint32_t dpi_index;
};

ZMK_EVENT_DECLARE(zitaotech_trackpad_dpi_changed);

#ifdef __cplusplus
}
#endif

void zitaotech_trackpad_relay_arrow_tap(uint16_t usage);
