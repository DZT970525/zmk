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
 * stays down until the touch ends (no pointer event for the touch time), so
 * holding a mouse button and then typing does not bounce straight back into
 * the mouse layer.
 */
static atomic_t mouse_layer_blocked = ATOMIC_INIT(0);

/*
 * How long a normal key keeps the mouse layer away.  Long enough that typing
 * does not fall back into the mouse layer between keystrokes, short enough
 * that the layer is usable again while the hand stays on the pointing device.
 */
#define MOUSE_LAYER_KEY_GRACE_MS 1000

/* Drop the automatic mouse layer without waiting for the touch to end. */
static void mouse_layer_drop_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    if (zmk_keymap_layer_active(CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER)) {
        /* ZMK main added a second `locking` argument to the layer API. */
        zmk_keymap_layer_deactivate(CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER, false);
    }
}

static K_WORK_DEFINE(mouse_layer_drop_work, mouse_layer_drop_work_cb);

/* The finger has left the pad: the layer is allowed back in. */
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
static atomic_t smooth_scroll_enabled = ATOMIC_INIT(0);
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
            CONFIG_ZITAOTECH_TRACKPAD_SCROLL_KEY_POSITION                                          \
    }

static const char *const mode_key_setting_names[ZITAOTECH_TRACKPAD_KEY_COUNT] = {
    "arrowkey",
    "fastkey",
    "slowkey",
    "scrollkey",
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
static void persist_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    persist_all();
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

static void mark_changed(void) {
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
    if (index >= ZITAOTECH_TRACKPAD_DPI_LEVELS) {
        return -EINVAL;
    }
    atomic_set(&dpi_index, (atomic_val_t)index);
    mark_changed();
    raise_dpi_changed();
    return 0;
}

int zitaotech_trackpad_step_dpi(int delta) {
    uint32_t next = clamp_dpi_index((int32_t)zitaotech_trackpad_dpi_index() + delta * (10 / DPI_STEP));
    atomic_set(&dpi_index, (atomic_val_t)next);
    mark_changed();
    raise_dpi_changed();
    return (int)next;
}

bool zitaotech_trackpad_burst_enabled(void) { return atomic_get(&burst_enabled) != 0; }

uint32_t zitaotech_trackpad_burst_multiplier(void) {
    return (uint32_t)atomic_get(&burst_multiplier);
}

uint32_t zitaotech_trackpad_pointer_multiplier(void) {
    return zitaotech_trackpad_burst_enabled() ? zitaotech_trackpad_burst_multiplier() : 1;
}

int zitaotech_trackpad_set_burst(bool enabled, uint32_t multiplier) {
    if (multiplier < 1 || multiplier > CONFIG_ZITAOTECH_TRACKPAD_BURST_MAX) {
        return -EINVAL;
    }
    atomic_set(&burst_enabled, enabled ? 1 : 0);
    atomic_set(&burst_multiplier, (atomic_val_t)multiplier);
    mark_changed();
    return 0;
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
    mark_changed();
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
    mark_changed();
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
    mark_changed();
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
    mark_changed();
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
    return dev == TEMP_LAYER_TRACKPAD_DEV;
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

    if (zmk_keymap_layer_active(CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER)) {
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
     * Only from the base layer: the mouse buttons are meant to be used while
     * typing on layer 0, and the overlay must not sit on top of the symbol,
     * navigation or media layers while the user holds one of them.  The mouse
     * layer itself may of course stay active once it is up.
     *
     * ZMK does not store the default layer in the state bitmask (it is always
     * active), so add it back before comparing.
     */
    zmk_keymap_layers_state_t base = BIT(zmk_keymap_layer_default());
    zmk_keymap_layers_state_t active = zmk_keymap_layer_state() | base;

    if ((active & ~BIT(CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER)) != base) {
        return false;
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
 * is exactly "the user pressed something that is not a mouse button": Caps
 * Lock, a modifier, a letter, a media key, ...
 */
static int mouse_layer_key_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);

    if (ev == NULL || !ev->state) {
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
        !zmk_keymap_layer_active(CONFIG_ZITAOTECH_TRACKPAD_MOUSE_LAYER)) {
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
    mark_changed();
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
    mark_changed();
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
    atomic_set(&smooth_scroll_enabled, 0);
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
 * Caps Lock for hosts that never report it.
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
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

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
