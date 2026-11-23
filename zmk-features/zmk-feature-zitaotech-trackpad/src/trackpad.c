#include <zmk/pointing_profiles.h>
/*
 * ZitaoTech trackpad runtime settings.
 *
 * Holds the values the Studio web UI can change (pointer speed, scroll speed,
 * scroll direction), persists them with ZMK's settings subsystem and hands
 * them to the board's trackpad driver. The driver (a320.c on Q10) is the only
 * consumer, so everything here is a handful of atomics plus save plumbing.
 *
 * Copyright (c) 2026 ZitaoTech
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(zitaotech_trackpad, CONFIG_ZMK_LOG_LEVEL);

#include <zitaotech/zmk/trackpad.h>

#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/hid_indicators.h>
#include <zmk/keymap.h>

#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER_CONTROL)
#include <zmk/pointing/temp_layer.h>

/*
 * Set when a normal key was pressed while the mouse layer was up.  The layer
 * stays down for the key grace period, so typing does not immediately bounce
 * back into the mouse layer when the pointing device is still active.
 */
static atomic_t mouse_layer_blocked = ATOMIC_INIT(0);

/*
 * After a normal key exits the mouse layer, allow new pointer events to
 * reactivate it after 200 ms. Pointer events must not extend this deadline.
 */
#define MOUSE_LAYER_KEY_GRACE_MS 200

/* Drop the automatic mouse layer without waiting for the touch to end. */
static void mouse_layer_drop_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    uint8_t layer_id = zmk_keymap_layer_index_to_id(CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER);
    if (zmk_keymap_layer_active(layer_id)) {
        /* ZMK main added a second `locking` argument to the layer API. */
        zmk_keymap_layer_deactivate(layer_id, false);
    }
}

static K_WORK_DEFINE(mouse_layer_drop_work, mouse_layer_drop_work_cb);

/* The key grace period expired: pointer events may activate the layer again. */
static void mouse_layer_unblock_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    atomic_set(&mouse_layer_blocked, 0);
}

static K_WORK_DELAYABLE_DEFINE(mouse_layer_unblock_work, mouse_layer_unblock_work_cb);

/*
 * Which pointing device is holding the mouse layer up right now.  The left
 * half's trackpad LED follows this, so it lights up for the pad only.
 */
static atomic_t mouse_layer_from_trackpad = ATOMIC_INIT(0);
#endif

ZMK_EVENT_IMPL(zitaotech_trackpad_dpi_changed);
ZMK_EVENT_IMPL(zitaotech_trackpad_mouse_layer_source_changed);

#define SETTINGS_PREFIX "ztp"

#define DPI_MIN CONFIG_ZITAOTECH_TRACKPAD_DPI_MIN
#define DPI_MAX CONFIG_ZITAOTECH_TRACKPAD_DPI_MAX
#define DPI_STEP CONFIG_ZITAOTECH_TRACKPAD_DPI_STEP
#define DPI_DEFAULT CONFIG_ZITAOTECH_TRACKPAD_DPI_DEFAULT

#define SPEED_MIN CONFIG_ZITAOTECH_TRACKPAD_SCROLL_SPEED_MIN
#define SPEED_MAX CONFIG_ZITAOTECH_TRACKPAD_SCROLL_SPEED_MAX
#define SPEED_DEFAULT CONFIG_ZITAOTECH_TRACKPAD_SCROLL_SPEED_DEFAULT

BUILD_ASSERT(DPI_STEP > 0, "DPI step must be positive");
BUILD_ASSERT(DPI_MAX >= DPI_MIN, "DPI range is inverted");
BUILD_ASSERT(SPEED_MAX >= SPEED_MIN, "scroll speed range is inverted");

/* dpi_index is stored rather than the raw value so the fine steps stay stable
 * even if the range in Kconfig changes later. */
static atomic_t dpi_index = ATOMIC_INIT((DPI_DEFAULT - DPI_MIN) / DPI_STEP);
static atomic_t scroll_speed_percent = ATOMIC_INIT(SPEED_DEFAULT);
static atomic_t scroll_invert = ATOMIC_INIT(0);
static atomic_t burst_enabled = ATOMIC_INIT(0);
static atomic_t burst_multiplier = ATOMIC_INIT(1);
static atomic_t scroll_burst_enabled = ATOMIC_INIT(0);
static atomic_t scroll_burst_multiplier = ATOMIC_INIT(1);
static atomic_t smooth_scroll_enabled = ATOMIC_INIT(IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_SMOOTH_SCROLL_DEFAULT));
/* Caps Lock scrolls on the stock Q10; this is on unless Studio turns it off. */
static atomic_t caps_lock_scroll_enabled = ATOMIC_INIT(1);
/* Automatic mouse layer: off unless the board asks for it (the Q20 does),
 * switched on or off from Studio. */
#define MOUSE_LAYER_DEFAULT_ON IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER_DEFAULT)
#define TRACKPOINT_MOUSE_LAYER_DEFAULT_ON                                                          \
    IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_TRACKPOINT_MOUSE_LAYER_DEFAULT)
static atomic_t mouse_layer_enabled = ATOMIC_INIT(MOUSE_LAYER_DEFAULT_ON);
/* How long the layer stays up after the last movement from each source. */
static atomic_t mouse_layer_ms = ATOMIC_INIT(CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER_MS);
static atomic_t trackpoint_mouse_layer_enabled = ATOMIC_INIT(TRACKPOINT_MOUSE_LAYER_DEFAULT_ON);
static atomic_t trackpoint_mouse_layer_ms =
    ATOMIC_INIT(CONFIG_ZITAOTECH_TRACKPAD_TRACKPOINT_MOUSE_LAYER_MS);
static atomic_t touch_on_ms = ATOMIC_INIT(CONFIG_ZITAOTECH_TRACKPAD_TOUCH_ON_MS);
static atomic_t arrow_swipe_threshold =
    ATOMIC_INIT(CONFIG_ZITAOTECH_TRACKPAD_ARROW_SWIPE_THRESHOLD);
/* Pointer mode is what these boards shipped with, and it stays the default. */
static atomic_t base_mode = ATOMIC_INIT(ZITAOTECH_TRACKPAD_MODE_POINTER);
/* The mode-switch key does nothing until Studio turns it on. */
static atomic_t mode_switch_enabled = ATOMIC_INIT(0);

#define MODE_KEY_DEFAULTS                                                                          \
    {                                                                                              \
        CONFIG_ZITAOTECH_TRACKPAD_ARROW_KEY_POSITION, CONFIG_ZITAOTECH_TRACKPAD_FAST_KEY_POSITION, \
            CONFIG_ZITAOTECH_TRACKPAD_SLOW_KEY_POSITION,                                           \
            CONFIG_ZITAOTECH_TRACKPAD_SCROLL_KEY_POSITION, -1                                      \
    }

static const char *const mode_key_setting_names[ZITAOTECH_TRACKPAD_KEY_COUNT] = {
    "arrowkey",
    "fastkey",
    "slowkey",
    "scrollkey",
    "fastscrollkey",
};

static atomic_t mode_key_positions[ZITAOTECH_TRACKPAD_KEY_COUNT];

static void mode_keys_load_defaults(void) {
    static const int32_t defaults[ZITAOTECH_TRACKPAD_KEY_COUNT] = MODE_KEY_DEFAULTS;
    for (int i = 0; i < ZITAOTECH_TRACKPAD_KEY_COUNT; i++) {
        atomic_set(&mode_key_positions[i], (atomic_val_t)defaults[i]);
    }
}

#define INVERT_X_BIT 0
#define INVERT_Y_BIT 1

static inline uint32_t clamp_dpi_index(int32_t index) {
    if (index < 0) {
        return 0;
    }
    if (index > (int32_t)(ZITAOTECH_TRACKPAD_DPI_LEVELS - 1)) {
        return ZITAOTECH_TRACKPAD_DPI_LEVELS - 1;
    }
    return (uint32_t)index;
}

uint32_t zitaotech_trackpad_dpi_index(void) { return (uint32_t)atomic_get(&dpi_index); }

uint32_t zitaotech_trackpad_dpi_value(void) { return DPI_MIN + DPI_STEP * zitaotech_trackpad_dpi_index(); }

static void save_dpi(void) {
    uint16_t value = (uint16_t)zitaotech_trackpad_dpi_value();
    settings_save_one(SETTINGS_PREFIX "/dpi", &value, sizeof(value));
}

static void save_speed(void) {
    uint16_t value = (uint16_t)zitaotech_trackpad_scroll_speed_percent();
    settings_save_one(SETTINGS_PREFIX "/speed", &value, sizeof(value));
}

static void save_invert(void) {
    uint8_t value = (uint8_t)atomic_get(&scroll_invert);
    settings_save_one(SETTINGS_PREFIX "/invert", &value, sizeof(value));
}

static void save_burst(void) {
    uint8_t enabled = (uint8_t)(atomic_get(&burst_enabled) ? 1 : 0);
    uint8_t multiplier = (uint8_t)zitaotech_trackpad_burst_multiplier();
    settings_save_one(SETTINGS_PREFIX "/burst", &enabled, sizeof(enabled));
    settings_save_one(SETTINGS_PREFIX "/burstmul", &multiplier, sizeof(multiplier));
}

static void save_scroll_burst(void) {
    uint8_t enabled = (uint8_t)(atomic_get(&scroll_burst_enabled) ? 1 : 0);
    uint8_t multiplier = (uint8_t)zitaotech_trackpad_scroll_burst_multiplier();
    settings_save_one(SETTINGS_PREFIX "/sburst", &enabled, sizeof(enabled));
    settings_save_one(SETTINGS_PREFIX "/sburstmul", &multiplier, sizeof(multiplier));
}

static void save_smooth_scroll(void) {
    uint8_t enabled = (uint8_t)(atomic_get(&smooth_scroll_enabled) ? 1 : 0);
    settings_save_one(SETTINGS_PREFIX "/smooth", &enabled, sizeof(enabled));
}

static void save_caps_lock_scroll(void) {
    uint8_t enabled = (uint8_t)(atomic_get(&caps_lock_scroll_enabled) ? 1 : 0);
    settings_save_one(SETTINGS_PREFIX "/capslock", &enabled, sizeof(enabled));
}

static void save_mouse_layer(void) {
    uint8_t enabled = (uint8_t)(atomic_get(&mouse_layer_enabled) ? 1 : 0);
    settings_save_one(SETTINGS_PREFIX "/mouse", &enabled, sizeof(enabled));
}

static void save_mouse_layer_ms(void) {
    uint16_t value = (uint16_t)zitaotech_trackpad_mouse_layer_ms();
    settings_save_one(SETTINGS_PREFIX "/mousems", &value, sizeof(value));
}

static void save_trackpoint_mouse_layer(void) {
    uint8_t enabled = (uint8_t)(atomic_get(&trackpoint_mouse_layer_enabled) ? 1 : 0);
    settings_save_one(SETTINGS_PREFIX "/tpmouse", &enabled, sizeof(enabled));
}

static void save_trackpoint_mouse_layer_ms(void) {
    uint16_t value = (uint16_t)zitaotech_trackpad_trackpoint_mouse_layer_ms();
    settings_save_one(SETTINGS_PREFIX "/tpmousems", &value, sizeof(value));
}

static void save_touch_on_ms(void) {
    uint16_t value = (uint16_t)zitaotech_trackpad_touch_on_ms();
    settings_save_one(SETTINGS_PREFIX "/touchms", &value, sizeof(value));
}

static void save_arrow_swipe(void) {
    uint16_t value = (uint16_t)zitaotech_trackpad_arrow_swipe_threshold();
    settings_save_one(SETTINGS_PREFIX "/arrowthr", &value, sizeof(value));
}

static void save_mode(void) {
    uint8_t value = (uint8_t)zitaotech_trackpad_mode();
    settings_save_one(SETTINGS_PREFIX "/mode", &value, sizeof(value));
}

static void save_mode_switch(void) {
    uint8_t value = (uint8_t)(zitaotech_trackpad_mode_switch_enabled() ? 1 : 0);
    settings_save_one(SETTINGS_PREFIX "/modeswitch", &value, sizeof(value));
}

static void save_mode_key(uint8_t mode);

static void persist_all(void) {
    save_dpi();
    save_speed();
    save_invert();
    save_burst();
    save_scroll_burst();
    save_smooth_scroll();
    save_caps_lock_scroll();
    save_mouse_layer();
    save_mouse_layer_ms();
    save_trackpoint_mouse_layer();
    save_trackpoint_mouse_layer_ms();
    save_touch_on_ms();
    save_arrow_swipe();
    save_mode();
    save_mode_switch();
    for (uint8_t i = 0; i < ZITAOTECH_TRACKPAD_KEY_COUNT; i++) {
        save_mode_key(i);
    }
}

/*
 * Flash writes are debounced: turning the DPI up five steps with the &DPI keys
 * should not mean five flash writes, and the key path should never block on
 * one. Every change also pushes a Studio notification so the web UI follows
 * along live.
 */
/* DPI tuning only needs its own settings entry. */
static atomic_t persist_all_pending = ATOMIC_INIT(0);
static atomic_t tuning_dirty = ATOMIC_INIT(0);

static void persist_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    atomic_val_t dirty = atomic_set(&tuning_dirty, 0);
    if (atomic_set(&persist_all_pending, 0)) {
        persist_all();
    } else {
        save_dpi();
        if (dirty & BIT(0)) save_burst();
        if (dirty & BIT(1)) save_scroll_burst();
        if (dirty & BIT(2)) save_speed();
        if (dirty & BIT(3)) save_mouse_layer_ms();
        if (dirty & BIT(4)) save_trackpoint_mouse_layer_ms();
        if (dirty & BIT(5)) save_touch_on_ms();
        if (dirty & BIT(6)) save_arrow_swipe();
    }
}

K_WORK_DELAYABLE_DEFINE(persist_work, persist_work_cb);

__weak void zitaotech_trackpad_state_changed(void) {}

static atomic_t notify_suppressed = ATOMIC_INIT(0);

void zitaotech_trackpad_suppress_notifications(bool suppress) {
    atomic_set(&notify_suppressed, suppress ? 1 : 0);
}

static void notify_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    zitaotech_trackpad_state_changed();
}

/*
 * Never raise the Studio notification straight from the caller's context.
 *
 * The &DPI keys run in the keyboard's behaviour context, and sending the
 * custom notification from there deadlocked against the RPC transport: the
 * whole keyboard froze on the first DPI key press while ZitaoTech Studio was
 * connected.  (The Studio's own DPI writes were unaffected because
 * trackpad_handler.c suppresses notifications for them.)  Queue the
 * notification instead and let the work queue send it - the same way the
 * backlight module announces its per-layer effect changes.
 */
static K_WORK_DEFINE(notify_work, notify_work_handler);

/*
 * The mode switch key only changes one entry, so it gets its own save work
 * instead of rewriting the whole settings blob (see set_mode below).
 */
static void mode_persist_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    save_mode();
}

K_WORK_DELAYABLE_DEFINE(mode_persist_work, mode_persist_work_handler);

static void mark_dpi_changed(void) {
    k_work_reschedule(&persist_work, K_MSEC(CONFIG_ZITAOTECH_TRACKPAD_SAVE_DEBOUNCE_MS));
    if (!atomic_get(&notify_suppressed)) {
        k_work_submit(&notify_work);
    }
}

static void mark_tuning_changed(uint8_t group) {
    atomic_or(&tuning_dirty, BIT(group));
    mark_dpi_changed();
}

static void mark_changed(void) {
    atomic_set(&persist_all_pending, 1);
    k_work_reschedule(&persist_work, K_MSEC(CONFIG_ZITAOTECH_TRACKPAD_SAVE_DEBOUNCE_MS));
    if (!atomic_get(&notify_suppressed)) {
        k_work_submit(&notify_work);
    }
}

static void raise_dpi_changed(void) {
    raise_zitaotech_trackpad_dpi_changed((struct zitaotech_trackpad_dpi_changed){
        .dpi_value = zitaotech_trackpad_dpi_value(),
        .dpi_index = zitaotech_trackpad_dpi_index(),
    });
}

int zitaotech_trackpad_set_dpi_index(uint32_t index) {
    if (zmk_pointing_profile_begin) zmk_pointing_profile_begin();
    if (index >= ZITAOTECH_TRACKPAD_DPI_LEVELS) {
        { if (zmk_pointing_profile_end) zmk_pointing_profile_end(); return -EINVAL; }
    }
    atomic_set(&dpi_index, (atomic_val_t)index);
    mark_dpi_changed();
    raise_dpi_changed();
    { if (zmk_pointing_profile_end) zmk_pointing_profile_end(); return 0; }
}

int zitaotech_trackpad_step_dpi(int delta) {
    if (zmk_pointing_profile_begin) zmk_pointing_profile_begin();
    uint32_t next = clamp_dpi_index((int32_t)zitaotech_trackpad_dpi_index() + delta * (10 / DPI_STEP));
    atomic_set(&dpi_index, (atomic_val_t)next);
    mark_dpi_changed();
    raise_dpi_changed();
    { if (zmk_pointing_profile_end) zmk_pointing_profile_end(); return (int)next; }
}

bool zitaotech_trackpad_burst_enabled(void) { return atomic_get(&burst_enabled) != 0; }

uint32_t zitaotech_trackpad_burst_multiplier(void) {
    return (uint32_t)atomic_get(&burst_multiplier);
}

uint32_t zitaotech_trackpad_pointer_multiplier(void) {
    return zitaotech_trackpad_burst_enabled() ? zitaotech_trackpad_burst_multiplier() : 1;
}

int zitaotech_trackpad_set_burst(bool enabled, uint32_t multiplier) {
    if (zmk_pointing_profile_begin) zmk_pointing_profile_begin();
    if (multiplier < 1 || multiplier > CONFIG_ZITAOTECH_TRACKPAD_BURST_MAX) {
        { if (zmk_pointing_profile_end) zmk_pointing_profile_end(); return -EINVAL; }
    }
    atomic_set(&burst_enabled, enabled ? 1 : 0);
    atomic_set(&burst_multiplier, (atomic_val_t)multiplier);
    mark_tuning_changed(0);
    { if (zmk_pointing_profile_end) zmk_pointing_profile_end(); return 0; }
}

bool zitaotech_trackpad_scroll_burst_enabled(void) {
    return atomic_get(&scroll_burst_enabled) != 0;
}

uint32_t zitaotech_trackpad_scroll_burst_multiplier(void) {
    return (uint32_t)atomic_get(&scroll_burst_multiplier);
}

uint32_t zitaotech_trackpad_scroll_multiplier(void) {
    return zitaotech_trackpad_scroll_burst_enabled() ? zitaotech_trackpad_scroll_burst_multiplier()
                                                     : 1;
}

int zitaotech_trackpad_set_scroll_burst(bool enabled, uint32_t multiplier) {
    if (multiplier < 1 || multiplier > CONFIG_ZITAOTECH_TRACKPAD_SCROLL_BURST_MAX) {
        return -EINVAL;
    }
    atomic_set(&scroll_burst_enabled, enabled ? 1 : 0);
    atomic_set(&scroll_burst_multiplier, (atomic_val_t)multiplier);
    mark_tuning_changed(1);
    return 0;
}

bool zitaotech_trackpad_smooth_scroll_enabled(void) {
    return atomic_get(&smooth_scroll_enabled) != 0;
}

int zitaotech_trackpad_set_smooth_scroll(bool enabled) {
    atomic_set(&smooth_scroll_enabled, enabled ? 1 : 0);
    mark_changed();
    return 0;
}

bool zitaotech_trackpad_caps_lock_scroll_enabled(void) {
    return atomic_get(&caps_lock_scroll_enabled) != 0;
}

int zitaotech_trackpad_set_caps_lock_scroll(bool enabled) {
    atomic_set(&caps_lock_scroll_enabled, enabled ? 1 : 0);
    mark_changed();
    return 0;
}

/* ---------------------------------------------------------------------------
 * Automatic mouse layer
 *
 * The layer itself is driven by ZMK's temporary layer input processor
 * (&zip_temp_layer <layer> <timeout>): the board binds it to the pointing
 * device, so the layer comes up as soon as the trackpad reports movement and
 * goes away again once there has been no movement for the timeout.  Two hooks
 * let this module decide whether that may happen at all and how long the
 * timeout is, so Studio can switch the feature off and change the time.
 * ------------------------------------------------------------------------ */

bool zitaotech_trackpad_mouse_layer_enabled(void) {
    return atomic_get(&mouse_layer_enabled) != 0;
}

int zitaotech_trackpad_set_mouse_layer(bool enabled) {
    atomic_set(&mouse_layer_enabled, enabled ? 1 : 0);
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER_EXIT_ON_KEY)
    /* Switching the feature back on should not inherit an old block. */
    if (!enabled) {
        k_work_cancel_delayable(&mouse_layer_unblock_work);
        atomic_set(&mouse_layer_blocked, 0);
    }
#endif
    mark_changed();
    return 0;
}

uint32_t zitaotech_trackpad_mouse_layer_ms(void) { return (uint32_t)atomic_get(&mouse_layer_ms); }

static uint32_t clamp_mouse_layer_ms(uint32_t ms) {
    if (ms < CONFIG_ZITAOTECH_TRACKPAD_TOUCH_ON_MIN) {
        return CONFIG_ZITAOTECH_TRACKPAD_TOUCH_ON_MIN;
    }
    if (ms > CONFIG_ZITAOTECH_TRACKPAD_TOUCH_ON_MAX) {
        return CONFIG_ZITAOTECH_TRACKPAD_TOUCH_ON_MAX;
    }
    return ms;
}

int zitaotech_trackpad_set_mouse_layer_ms(uint32_t ms) {
    atomic_set(&mouse_layer_ms, (atomic_val_t)clamp_mouse_layer_ms(ms));
    mark_tuning_changed(3);
    return 0;
}

bool zitaotech_trackpad_trackpoint_mouse_layer_enabled(void) {
    return atomic_get(&trackpoint_mouse_layer_enabled) != 0;
}

int zitaotech_trackpad_set_trackpoint_mouse_layer(bool enabled) {
    atomic_set(&trackpoint_mouse_layer_enabled, enabled ? 1 : 0);
    mark_changed();
    return 0;
}

uint32_t zitaotech_trackpad_trackpoint_mouse_layer_ms(void) {
    return (uint32_t)atomic_get(&trackpoint_mouse_layer_ms);
}

int zitaotech_trackpad_set_trackpoint_mouse_layer_ms(uint32_t ms) {
    atomic_set(&trackpoint_mouse_layer_ms, (atomic_val_t)clamp_mouse_layer_ms(ms));
    mark_tuning_changed(4);
    return 0;
}

uint32_t zitaotech_trackpad_touch_on_ms(void) {
    return (uint32_t)atomic_get(&touch_on_ms);
}

int zitaotech_trackpad_set_touch_on_ms(uint32_t ms) {
    if (ms < CONFIG_ZITAOTECH_TRACKPAD_TOUCH_ON_MIN || ms > CONFIG_ZITAOTECH_TRACKPAD_TOUCH_ON_MAX) {
        return -EINVAL;
    }
    atomic_set(&touch_on_ms, (atomic_val_t)ms);
    /* The trackpad LED reads the value when it schedules its own timeout. */
    mark_tuning_changed(5);
    return 0;
}

#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER_CONTROL)

/*
 * Which of the two pointing devices an event came from.  The trackpad is the
 * local A320; everything else that feeds the mouse layer is the TrackPoint
 * half, whose motion arrives through the split input listener.
 */
#if DT_HAS_COMPAT_STATUS_OKAY(avago_a320)
#define TEMP_LAYER_TRACKPAD_DEV DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(avago_a320))

static bool temp_layer_source_is_trackpad(const struct device *dev) {
#if IS_ENABLED(CONFIG_SOFLE_MODULAR_POINTING) && DT_NODE_HAS_STATUS(DT_NODELABEL(bbtrackpad_split), okay)
    if (dev == DEVICE_DT_GET(DT_NODELABEL(bbtrackpad_split))) return true;
#endif
#if IS_ENABLED(CONFIG_SOFLE_MODULAR_POINTING) && DT_NODE_HAS_STATUS(DT_NODELABEL(right_trackpad_split), okay)
    if (dev == DEVICE_DT_GET(DT_NODELABEL(right_trackpad_split))) return true;
#endif
    #if IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE_LEFT)
    return dev == TEMP_LAYER_TRACKPAD_DEV || dev == DEVICE_DT_GET(DT_NODELABEL(bbtrackpad_split));
#else
    return dev == TEMP_LAYER_TRACKPAD_DEV;
#endif
}
#elif (defined(CONFIG_BOARD_KEYPOINT_DONGLE) || defined(CONFIG_BOARD_SOFLE_DONGLE))
static bool temp_layer_source_is_trackpad(const struct device *dev) {
    return dev == DEVICE_DT_GET(DT_NODELABEL(bbtrackpad_split));
}
#else
static bool temp_layer_source_is_trackpad(const struct device *dev) {
    /* No A320 in this build: everything that reaches the mouse layer is the
     * TrackPoint half. */
    ARG_UNUSED(dev);
    return false;
}
#endif

static bool temp_layer_source_enabled(const struct device *dev) {
    return temp_layer_source_is_trackpad(dev) ? zitaotech_trackpad_mouse_layer_enabled()
                                              : zitaotech_trackpad_trackpoint_mouse_layer_enabled();
}

static uint32_t temp_layer_source_ms(const struct device *dev) {
    return temp_layer_source_is_trackpad(dev) ? zitaotech_trackpad_mouse_layer_ms()
                                              : zitaotech_trackpad_trackpoint_mouse_layer_ms();
}

bool zitaotech_trackpad_mouse_layer_from_trackpad(void) {
    return atomic_get(&mouse_layer_from_trackpad) != 0;
}

/*
 * Remember which device is driving the layer, and tell anyone listening (the
 * trackpad LED) when that changes while the layer stays up - moving the
 * TrackPoint while a finger rests on the pad must not leave the pad lit.
 */
static void note_mouse_layer_source(bool from_trackpad) {
    atomic_val_t previous = atomic_set(&mouse_layer_from_trackpad, from_trackpad ? 1 : 0);
    if ((previous != 0) == from_trackpad) {
        return;
    }

    if (zmk_keymap_layer_active(zmk_keymap_layer_index_to_id(CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER))) {
        raise_zitaotech_trackpad_mouse_layer_source_changed(
            (struct zitaotech_trackpad_mouse_layer_source_changed){
                .from_trackpad = from_trackpad,
            });
    }
}

/* Overrides the weak defaults in ZMK's temporary layer input processor. */
bool zmk_input_processor_temp_layer_enabled(const struct device *dev, uint32_t layer) {
    if (layer != (uint32_t)CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER) {
        return true;
    }

    if (!temp_layer_source_enabled(dev)) {
        return false;
    }

    /*
     * Just after a normal key was pressed the layer stays down for a moment
     * (MOUSE_LAYER_KEY_GRACE_MS), so typing does not bounce straight back into
     * the mouse layer.  The wait is deliberately *not* extended by pointer
     * events: a hand resting on the pad or the stick keeps reporting "nothing
     * moved" packets, and pushing the deadline out on those meant the layer
     * could not come back until the hand was lifted - which is exactly the
     * behaviour that made a double click on the mouse buttons feel broken.
     */
    if (atomic_get(&mouse_layer_blocked)) {
        return false;
    }

    /*
     * Legacy base-only policy, retained on boards without the all-layers option:
     * the mouse buttons are meant to be used while
     * typing on layer 0, and the overlay must not sit on top of the symbol,
     * navigation or media layers while the user holds one of them.  The mouse
     * layer itself may of course stay active once it is up.
     *
     * ZMK does not store the default layer in the state bitmask (it is always
     * active), so add it back before comparing.
     */
    if (!zmk_input_processor_temp_layer_overlay_enabled()) {
        zmk_keymap_layers_state_t base = BIT(zmk_keymap_layer_default());
        zmk_keymap_layers_state_t active = zmk_keymap_layer_state() | base;
        uint8_t mouse_id = zmk_keymap_layer_index_to_id(CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER);
        if ((active & ~BIT(mouse_id)) != base) {
            return false;
        }
    }

    /* This event is allowed to raise or hold the layer, so it is the current
     * source as far as the LED is concerned. */
    note_mouse_layer_source(temp_layer_source_is_trackpad(dev));
    return true;
}

uint32_t zmk_input_processor_temp_layer_timeout_ms(const struct device *dev, uint32_t layer,
                                                   uint32_t timeout_ms) {
    if (layer == (uint32_t)CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER) {
        return temp_layer_source_ms(dev);
    }
    return timeout_ms;
}

#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER_EXIT_ON_KEY)
/*
 * Going back to typing leaves the mouse layer.  Mouse buttons never produce a
 * keycode - &mkp drives the HID mouse report directly - so "a keycode arrived"
 * normally indicates typing. Encoder-generated keyboard/media keycodes are
 * excluded: using a mouse-layer rotary binding must not dismiss that layer.
 */
__weak bool zmk_keypoint_mouse_layer_exit_on_key_enabled(void) { return true; }
static int mouse_layer_key_listener(const zmk_event_t *eh) {
    if (!zmk_keypoint_mouse_layer_exit_on_key_enabled()) return ZMK_EV_EVENT_BUBBLE;
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);

    if (ev == NULL || !ev->state || ev->from_sensor) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /*
     * Direction mode turns the pad into an arrow key generator, and those
     * arrows arrive here as ordinary keycodes (the pad's input processors fire
     * them).  They are the pad's own output, not typing, so they must not drop
     * the mouse layer - otherwise the layer could never come up while the pad
     * is in direction mode.
     */
    if (ev->usage_page == HID_USAGE_KEY && ev->implicit_modifiers == 0 &&
        ev->explicit_modifiers == 0 &&
        (ev->keycode == HID_USAGE_KEY_KEYBOARD_LEFTARROW ||
         ev->keycode == HID_USAGE_KEY_KEYBOARD_RIGHTARROW ||
         ev->keycode == HID_USAGE_KEY_KEYBOARD_UPARROW ||
         ev->keycode == HID_USAGE_KEY_KEYBOARD_DOWNARROW)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!zitaotech_trackpad_mouse_layer_enabled() ||
        !zmk_keymap_layer_active(zmk_keymap_layer_index_to_id(CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER))) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    atomic_set(&mouse_layer_blocked, 1);
    /* The block is time based, not touch based: it has to clear even while a
     * hand keeps resting on the pointing device. */
    k_work_reschedule(&mouse_layer_unblock_work, K_MSEC(MOUSE_LAYER_KEY_GRACE_MS));
    k_work_submit(&mouse_layer_drop_work);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zitaotech_trackpad_mouse_layer, mouse_layer_key_listener);
ZMK_SUBSCRIPTION(zitaotech_trackpad_mouse_layer, zmk_keycode_state_changed);
#endif /* CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER_EXIT_ON_KEY */

#endif /* CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER_CONTROL */

#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
/* Overrides the weak default in ZMK's pointing input listener. */
bool zmk_pointing_smooth_scrolling_enabled(void) {
    return zitaotech_trackpad_smooth_scroll_enabled();
}
#endif

uint32_t zitaotech_trackpad_arrow_swipe_threshold(void) {
    return (uint32_t)atomic_get(&arrow_swipe_threshold);
}

int zitaotech_trackpad_set_arrow_swipe_threshold(uint32_t threshold) {
    if (threshold < CONFIG_ZITAOTECH_TRACKPAD_ARROW_SWIPE_MIN ||
        threshold > CONFIG_ZITAOTECH_TRACKPAD_ARROW_SWIPE_MAX) {
        return -EINVAL;
    }
    atomic_set(&arrow_swipe_threshold, (atomic_val_t)threshold);
    mark_tuning_changed(6);
    return 0;
}

uint32_t zitaotech_trackpad_mode(void) { return (uint32_t)atomic_get(&base_mode); }

int zitaotech_trackpad_set_mode(uint32_t mode) {
    if (mode > ZITAOTECH_TRACKPAD_MODE_ARROW) {
        return -EINVAL;
    }

    atomic_set(&base_mode, (atomic_val_t)mode);

    /*
     * Only the mode changed.  persist_all() rewrites every trackpad setting
     * (~17 NVS entries) and the flash write stalls the CPU long enough to be
     * felt as a hitch right after the mode switch key - so save just this one
     * entry, and still tell Studio so the web UI follows along.
     */
    k_work_reschedule(&mode_persist_work, K_MSEC(CONFIG_ZITAOTECH_TRACKPAD_SAVE_DEBOUNCE_MS));
    if (!atomic_get(&notify_suppressed)) {
        k_work_submit(&notify_work);
    }

    return 0;
}

bool zitaotech_trackpad_mode_switch_enabled(void) {
    return atomic_get(&mode_switch_enabled) != 0;
}

int zitaotech_trackpad_set_mode_switch_enabled(bool enabled) {
    atomic_set(&mode_switch_enabled, enabled ? 1 : 0);
    mark_changed();
    return 0;
}

int zitaotech_trackpad_toggle_mode(void) {
    if (!zitaotech_trackpad_mode_switch_enabled()) {
        return -EACCES;
    }

    uint32_t next = (zitaotech_trackpad_mode() == ZITAOTECH_TRACKPAD_MODE_ARROW)
                        ? ZITAOTECH_TRACKPAD_MODE_POINTER
                        : ZITAOTECH_TRACKPAD_MODE_ARROW;
    int rc = zitaotech_trackpad_set_mode(next);
    return rc < 0 ? rc : (int)next;
}

static int32_t mode_key_get(enum zitaotech_trackpad_mode_key mode) {
    if (mode >= ZITAOTECH_TRACKPAD_KEY_COUNT) {
        return -1;
    }
    return (int32_t)atomic_get(&mode_key_positions[mode]);
}

int32_t zitaotech_trackpad_arrow_key_position(void) {
    return mode_key_get(ZITAOTECH_TRACKPAD_KEY_ARROW);
}

int32_t zitaotech_trackpad_fast_key_position(void) {
    return mode_key_get(ZITAOTECH_TRACKPAD_KEY_FAST);
}

int32_t zitaotech_trackpad_slow_key_position(void) {
    return mode_key_get(ZITAOTECH_TRACKPAD_KEY_SLOW);
}

int32_t zitaotech_trackpad_fast_scroll_key_position(void) {
    return mode_key_get(ZITAOTECH_TRACKPAD_KEY_FAST_SCROLL);
}

int32_t zitaotech_trackpad_scroll_key_position(void) {
    return mode_key_get(ZITAOTECH_TRACKPAD_KEY_SCROLL);
}

int zitaotech_trackpad_set_mode_key(uint8_t mode, int32_t position) {
    if (mode >= ZITAOTECH_TRACKPAD_KEY_COUNT || position < -1) {
        return -EINVAL;
    }
    atomic_set(&mode_key_positions[mode], (atomic_val_t)position);
    mark_changed();
    return 0;
}

int zitaotech_trackpad_set_mode_keys(const int32_t positions[ZITAOTECH_TRACKPAD_KEY_COUNT]) {
    for (uint8_t i = 0; i < ZITAOTECH_TRACKPAD_KEY_COUNT; i++) {
        if (positions[i] < -1) {
            return -EINVAL;
        }
        for (uint8_t j = i + 1; j < ZITAOTECH_TRACKPAD_KEY_COUNT; j++) {
            if (positions[i] >= 0 && positions[i] == positions[j]) {
                /* One position, two modes: refuse the whole update. */
                return -EINVAL;
            }
        }
    }

    for (uint8_t i = 0; i < ZITAOTECH_TRACKPAD_KEY_COUNT; i++) {
        atomic_set(&mode_key_positions[i], (atomic_val_t)positions[i]);
    }
    mark_changed();
    return 0;
}

static void save_mode_key(uint8_t mode) {
    char path[32];
    int32_t value = mode_key_get((enum zitaotech_trackpad_mode_key)mode);
    snprintf(path, sizeof(path), SETTINGS_PREFIX "/%s", mode_key_setting_names[mode]);
    settings_save_one(path, &value, sizeof(value));
}

uint32_t zitaotech_trackpad_scroll_speed_percent(void) {
    return (uint32_t)atomic_get(&scroll_speed_percent);
}

int zitaotech_trackpad_set_scroll_speed_percent(uint32_t percent) {
    if (percent < SPEED_MIN || percent > SPEED_MAX) {
        return -EINVAL;
    }
    atomic_set(&scroll_speed_percent, (atomic_val_t)percent);
    mark_tuning_changed(2);
    return 0;
}

bool zitaotech_trackpad_scroll_invert_x(void) {
    return (atomic_get(&scroll_invert) & BIT(INVERT_X_BIT)) != 0;
}

bool zitaotech_trackpad_scroll_invert_y(void) {
    return (atomic_get(&scroll_invert) & BIT(INVERT_Y_BIT)) != 0;
}

int zitaotech_trackpad_set_scroll_invert(bool invert_x, bool invert_y) {
    atomic_val_t value = 0;
    if (invert_x) {
        value |= BIT(INVERT_X_BIT);
    }
    if (invert_y) {
        value |= BIT(INVERT_Y_BIT);
    }
    atomic_set(&scroll_invert, value);
    mark_changed();
    return 0;
}

int zitaotech_trackpad_reset(void) {
    atomic_set(&dpi_index, (atomic_val_t)((DPI_DEFAULT - DPI_MIN) / DPI_STEP));
    atomic_set(&scroll_speed_percent, (atomic_val_t)SPEED_DEFAULT);
    atomic_set(&scroll_invert, 0);
    atomic_set(&burst_enabled, 0);
    atomic_set(&burst_multiplier, 1);
    atomic_set(&scroll_burst_enabled, 0);
    atomic_set(&scroll_burst_multiplier, 1);
    atomic_set(&smooth_scroll_enabled, IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_SMOOTH_SCROLL_DEFAULT));
    atomic_set(&caps_lock_scroll_enabled, 1);
    atomic_set(&mouse_layer_enabled, MOUSE_LAYER_DEFAULT_ON);
    atomic_set(&mouse_layer_ms, CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER_MS);
    atomic_set(&trackpoint_mouse_layer_enabled, TRACKPOINT_MOUSE_LAYER_DEFAULT_ON);
    atomic_set(&trackpoint_mouse_layer_ms,
               CONFIG_ZITAOTECH_TRACKPAD_TRACKPOINT_MOUSE_LAYER_MS);
    atomic_set(&touch_on_ms, CONFIG_ZITAOTECH_TRACKPAD_TOUCH_ON_MS);
    atomic_set(&arrow_swipe_threshold, CONFIG_ZITAOTECH_TRACKPAD_ARROW_SWIPE_THRESHOLD);
    atomic_set(&base_mode, ZITAOTECH_TRACKPAD_MODE_POINTER);
    atomic_set(&mode_switch_enabled, 0);
    mode_keys_load_defaults();
    mark_changed();
    raise_dpi_changed();
    return 0;
}

int zitaotech_trackpad_save(void) {
    k_work_cancel_delayable(&persist_work);
    persist_all();
    return 0;
}

static int trackpad_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                 void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "dpi", &next) && !next) {
        uint16_t raw = 0;
        int rc;
        if (len == sizeof(uint8_t)) {
            /* V18 and earlier stored an index into 10,20,...100 DPI. */
            uint8_t legacy;
            rc = read_cb(cb_arg, &legacy, sizeof(legacy));
            if (rc < 0) return rc;
            raw = 10 + 10 * legacy;
        } else if (len == sizeof(raw)) {
            rc = read_cb(cb_arg, &raw, sizeof(raw));
            if (rc < 0) return rc;
        } else {
            return -EINVAL;
        }
        atomic_set(&dpi_index, clamp_dpi_index(((int32_t)raw - DPI_MIN) / DPI_STEP));
        return 0;
    }

    if (settings_name_steq(name, "speed", &next) && !next) {
        if (len != sizeof(uint16_t)) {
            return -EINVAL;
        }
        uint16_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        if (value < SPEED_MIN || value > SPEED_MAX) {
            value = SPEED_DEFAULT;
        }
        atomic_set(&scroll_speed_percent, (atomic_val_t)value);
        return 0;
    }

    if (settings_name_steq(name, "invert", &next) && !next) {
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }
        uint8_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        atomic_set(&scroll_invert, (atomic_val_t)value);
        return 0;
    }

    if (settings_name_steq(name, "burst", &next) && !next) {
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }
        uint8_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        atomic_set(&burst_enabled, value ? 1 : 0);
        return 0;
    }

    if (settings_name_steq(name, "burstmul", &next) && !next) {
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }
        uint8_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        if (value < 1 || value > CONFIG_ZITAOTECH_TRACKPAD_BURST_MAX) {
            value = 1;
        }
        atomic_set(&burst_multiplier, (atomic_val_t)value);
        return 0;
    }

    if (settings_name_steq(name, "sburst", &next) && !next) {
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }
        uint8_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        atomic_set(&scroll_burst_enabled, value ? 1 : 0);
        return 0;
    }

    if (settings_name_steq(name, "sburstmul", &next) && !next) {
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }
        uint8_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        if (value < 1 || value > CONFIG_ZITAOTECH_TRACKPAD_SCROLL_BURST_MAX) {
            value = 1;
        }
        atomic_set(&scroll_burst_multiplier, (atomic_val_t)value);
        return 0;
    }

    if (settings_name_steq(name, "smooth", &next) && !next) {
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }
        uint8_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        atomic_set(&smooth_scroll_enabled, value ? 1 : 0);
        return 0;
    }

    if (settings_name_steq(name, "capslock", &next) && !next) {
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }
        uint8_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        atomic_set(&caps_lock_scroll_enabled, value ? 1 : 0);
        return 0;
    }

    if (settings_name_steq(name, "mouse", &next) && !next) {
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }
        uint8_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        atomic_set(&mouse_layer_enabled, value ? 1 : 0);
        return 0;
    }

    if (settings_name_steq(name, "touchms", &next) && !next) {
        if (len != sizeof(uint16_t)) {
            return -EINVAL;
        }
        uint16_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        if (value < CONFIG_ZITAOTECH_TRACKPAD_TOUCH_ON_MIN ||
            value > CONFIG_ZITAOTECH_TRACKPAD_TOUCH_ON_MAX) {
            value = CONFIG_ZITAOTECH_TRACKPAD_TOUCH_ON_MS;
        }
        atomic_set(&touch_on_ms, (atomic_val_t)value);
        return 0;
    }

    if (settings_name_steq(name, "mousems", &next) && !next) {
        if (len != sizeof(uint16_t)) {
            return -EINVAL;
        }
        uint16_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        atomic_set(&mouse_layer_ms, (atomic_val_t)clamp_mouse_layer_ms(value));
        return 0;
    }

    if (settings_name_steq(name, "tpmouse", &next) && !next) {
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }
        uint8_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        atomic_set(&trackpoint_mouse_layer_enabled, value ? 1 : 0);
        return 0;
    }

    if (settings_name_steq(name, "tpmousems", &next) && !next) {
        if (len != sizeof(uint16_t)) {
            return -EINVAL;
        }
        uint16_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        atomic_set(&trackpoint_mouse_layer_ms, (atomic_val_t)clamp_mouse_layer_ms(value));
        return 0;
    }

    if (settings_name_steq(name, "arrowthr", &next) && !next) {
        if (len != sizeof(uint16_t)) {
            return -EINVAL;
        }
        uint16_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        if (value < CONFIG_ZITAOTECH_TRACKPAD_ARROW_SWIPE_MIN ||
            value > CONFIG_ZITAOTECH_TRACKPAD_ARROW_SWIPE_MAX) {
            value = CONFIG_ZITAOTECH_TRACKPAD_ARROW_SWIPE_THRESHOLD;
        }
        atomic_set(&arrow_swipe_threshold, (atomic_val_t)value);
        return 0;
    }

    if (settings_name_steq(name, "mode", &next) && !next) {
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }
        uint8_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        if (value > ZITAOTECH_TRACKPAD_MODE_ARROW) {
            value = ZITAOTECH_TRACKPAD_MODE_POINTER;
        }
        atomic_set(&base_mode, (atomic_val_t)value);
        return 0;
    }

    if (settings_name_steq(name, "modeswitch", &next) && !next) {
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }
        uint8_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        atomic_set(&mode_switch_enabled, value ? 1 : 0);
        return 0;
    }

    for (uint8_t i = 0; i < ZITAOTECH_TRACKPAD_KEY_COUNT; i++) {
        if (!settings_name_steq(name, mode_key_setting_names[i], &next) || next) {
            continue;
        }
        if (len != sizeof(int32_t)) {
            return -EINVAL;
        }
        int32_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        if (value < -1) {
            value = -1;
        }
        atomic_set(&mode_key_positions[i], (atomic_val_t)value);
        return 0;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(zitaotech_trackpad, SETTINGS_PREFIX, NULL, trackpad_settings_set,
                               NULL, NULL);

/*
 * ---------------------------------------------------------------------------
 * Caps Lock for hosts that never report it. Q20 deliberately excludes this
 * fallback: its indicators must come from the connected host only.
 *
 * Caps Lock scroll mode - and the trackpad LED - follow the host's LED report,
 * which is the only way to know the real lock state.  Plenty of hosts simply
 * never send that report over BLE (Android is the usual offender), which left
 * Caps Lock doing nothing there while the same keyboard worked over USB.
 *
 * The lock state can only change in two ways: the host locks or unlocks it, or
 * this keyboard sends the Caps Lock key.  So whenever this keyboard sends that
 * key we set the stored indicator straight away - to the value the key just
 * produced, not a blind toggle, so a second press that races a host report
 * still lands on the right state.  On a host that does report, the report that
 * follows carries the same value, so nothing changes there; on a host that
 * stays quiet, the state we derive is the host's actual Caps Lock state,
 * because our key press is what toggled it.
 * ------------------------------------------------------------------------
 */
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS) && !IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) && \
    !IS_ENABLED(CONFIG_ZITAOTECH_Q10_HOST_CAPS_ONLY) && \
    (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

/* Same layout as the board drivers and the HID spec: bit 1 is Caps Lock. */
#define HID_INDICATORS_CAPS_LOCK (1 << 1)

static int caps_lock_key_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);

    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->usage_page != HID_USAGE_KEY || ev->keycode != HID_USAGE_KEY_KEYBOARD_CAPS_LOCK) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    zmk_hid_indicators_t indicators = zitaotech_caps_indicators();

    zmk_hid_indicators_set_profile(indicators ^ HID_INDICATORS_CAPS_LOCK,
                                   zitaotech_caps_endpoint());

    LOG_DBG("Caps Lock toggled locally, indicators now %x", indicators ^ HID_INDICATORS_CAPS_LOCK);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zitaotech_trackpad_caps_lock, caps_lock_key_listener);
ZMK_SUBSCRIPTION(zitaotech_trackpad_caps_lock, zmk_keycode_state_changed);

#endif /* IS_ENABLED(CONFIG_ZMK_HID_INDICATORS) */

static int zitaotech_trackpad_init(void) {
    mode_keys_load_defaults();
    LOG_INF("trackpad control: dpi=%u (%u..%u step %u, %u levels) speed=%u%%",
            (unsigned int)zitaotech_trackpad_dpi_value(), DPI_MIN, DPI_MAX, DPI_STEP,
            (unsigned int)ZITAOTECH_TRACKPAD_DPI_LEVELS,
            (unsigned int)zitaotech_trackpad_scroll_speed_percent());
    return 0;
}

SYS_INIT(zitaotech_trackpad_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* Peripheral gestures are emitted by the central endpoint. */
struct zitaotech_trackpad_arrow_tap { uint16_t usage; uint8_t source; };
ZMK_EVENT_DECLARE(zitaotech_trackpad_arrow_tap);
ZMK_EVENT_IMPL(zitaotech_trackpad_arrow_tap);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zitaotech_trackpad_arrow_tap, tpa, source);
ZMK_RELAY_EVENT_HANDLE(zitaotech_trackpad_arrow_tap, tpa, source);
void zitaotech_trackpad_relay_arrow_tap(uint16_t usage) {
    raise_zitaotech_trackpad_arrow_tap((struct zitaotech_trackpad_arrow_tap){
        .usage = usage, .source = ZMK_RELAY_EVENT_SOURCE_SELF});
}
#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE)) || IS_ENABLED(CONFIG_ZMK_SPLIT_RUNTIME_ROLE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE_LEFT) || IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE_LEFT)
#include <dt-bindings/zmk/keys.h>
#include <dt-bindings/zmk/modifiers.h>
static void central_arrow_tap(uint16_t usage) {
    if (zmk_hid_keyboard_is_pressed(usage)) {
        return;
    }

    struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    zmk_mod_flags_t saved_mods = report->body.modifiers;
    int ret = zmk_hid_keyboard_press(usage);
    if (ret < 0) {
        return;
    }

    report->body.modifiers = saved_mods & ~(MOD_LSFT | MOD_RSFT);
    int press_err = zmk_endpoint_send_report(HID_USAGE_KEY);
    zmk_hid_keyboard_release(usage);
    int release_err = zmk_endpoint_send_report(HID_USAGE_KEY);
    report->body.modifiers = saved_mods;

    if (saved_mods & (MOD_LSFT | MOD_RSFT)) {
        zmk_endpoint_send_report(HID_USAGE_KEY);
    }
    if (press_err < 0 || release_err < 0) {
        LOG_DBG("Arrow report failed: press=%d release=%d", press_err, release_err);
    }
}

K_MSGQ_DEFINE(remote_arrow_taps, sizeof(uint16_t), 8, 2);
static void remote_arrow_work_cb(struct k_work *work) {
    uint16_t usage;
    while (k_msgq_get(&remote_arrow_taps, &usage, K_NO_WAIT) == 0) central_arrow_tap(usage);
}
K_WORK_DEFINE(remote_arrow_work, remote_arrow_work_cb);
static int remote_arrow_listener(const zmk_event_t *eh) {
    const struct zitaotech_trackpad_arrow_tap *ev = as_zitaotech_trackpad_arrow_tap(eh);
    if (ev) {
        if (k_msgq_put(&remote_arrow_taps, &ev->usage, K_NO_WAIT) == 0) k_work_submit(&remote_arrow_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(zitaotech_remote_arrow, remote_arrow_listener);
ZMK_SUBSCRIPTION(zitaotech_remote_arrow, zitaotech_trackpad_arrow_tap);
#endif

/* Dongle owns DPI. The peripheral mirror is for LED feedback only: no flash writes. */
#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE)) || (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE_LEFT) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE_LEFT))
struct zitaotech_trackpad_led_dpi { uint8_t index; uint8_t request; uint8_t source; };
ZMK_EVENT_DECLARE(zitaotech_trackpad_led_dpi);
ZMK_EVENT_IMPL(zitaotech_trackpad_led_dpi);
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zitaotech_trackpad_led_dpi, tld, source);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zitaotech_trackpad_led_dpi, tld, source);
ZMK_RELAY_EVENT_HANDLE(zitaotech_trackpad_led_dpi, tld, source);

#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE))
static void led_dpi_send_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    raise_zitaotech_trackpad_led_dpi((struct zitaotech_trackpad_led_dpi){
        .index = zitaotech_trackpad_dpi_index(), .source = ZMK_RELAY_EVENT_SOURCE_SELF});
}
K_WORK_DELAYABLE_DEFINE(led_dpi_send_work, led_dpi_send_work_cb);
static int led_dpi_changed_listener(const zmk_event_t *eh) {
    if (as_zitaotech_trackpad_dpi_changed(eh)) k_work_reschedule(&led_dpi_send_work, K_MSEC(30));
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(dongle_led_dpi_changed, led_dpi_changed_listener);
ZMK_SUBSCRIPTION(dongle_led_dpi_changed, zitaotech_trackpad_dpi_changed);
#else
#include <zmk/events/split_peripheral_status_changed.h>
extern void indicator_tp_dpi_changed(void) __attribute__((weak));
static atomic_t led_dpi_received;
static uint8_t led_dpi_attempts;
static void led_dpi_request_work_cb(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(led_dpi_request_work, led_dpi_request_work_cb);
static void led_dpi_request_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    if (atomic_get(&led_dpi_received) || led_dpi_attempts >= 10) return;
    led_dpi_attempts++;
    raise_zitaotech_trackpad_led_dpi((struct zitaotech_trackpad_led_dpi){
        .request = 1, .source = ZMK_RELAY_EVENT_SOURCE_SELF});
    k_work_reschedule(&led_dpi_request_work, K_MSEC(500));
}
static int led_dpi_connection_listener(const zmk_event_t *eh) {
    const struct zmk_split_peripheral_status_changed *ev = as_zmk_split_peripheral_status_changed(eh);
    if (ev) {
        atomic_set(&led_dpi_received, 0);
        led_dpi_attempts = 0;
        if (ev->connected) k_work_reschedule(&led_dpi_request_work, K_MSEC(500));
        else k_work_cancel_delayable(&led_dpi_request_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(dongle_led_dpi_connection, led_dpi_connection_listener);
ZMK_SUBSCRIPTION(dongle_led_dpi_connection, zmk_split_peripheral_status_changed);
#endif
static int led_dpi_apply_listener(const zmk_event_t *eh) {
    const struct zitaotech_trackpad_led_dpi *ev = as_zitaotech_trackpad_led_dpi(eh);
    if (!ev || ev->source == ZMK_RELAY_EVENT_SOURCE_SELF) return ZMK_EV_EVENT_BUBBLE;
#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE))
    if (ev->request) k_work_reschedule(&led_dpi_send_work, K_NO_WAIT);
#else
    if (!ev->request && ev->index < ZITAOTECH_TRACKPAD_DPI_LEVELS) {
        atomic_set(&dpi_index, ev->index);
        atomic_set(&led_dpi_received, 1);
        if (indicator_tp_dpi_changed) indicator_tp_dpi_changed();
    }
#endif
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(dongle_led_dpi_apply, led_dpi_apply_listener);
ZMK_SUBSCRIPTION(dongle_led_dpi_apply, zitaotech_trackpad_led_dpi);
#endif
