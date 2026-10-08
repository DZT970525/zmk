/*
 * TrackPoint pointer speed (ten DPI steps) and burst mode.
 *
 * Copyright (c) 2026 ZitaoTech
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include <zitaotech/zmk/trackpoint.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
#include <zmk/split/central.h>
#endif

#define SETTINGS_PREFIX "zitaotech_trackpoint"

#define DPI_MIN CONFIG_ZITAOTECH_TRACKPOINT_DPI_MIN
#define DPI_MAX CONFIG_ZITAOTECH_TRACKPOINT_DPI_MAX
#define DPI_STEP CONFIG_ZITAOTECH_TRACKPOINT_DPI_STEP
#define DPI_DEFAULT CONFIG_ZITAOTECH_TRACKPOINT_DPI_DEFAULT
#define BURST_MAX CONFIG_ZITAOTECH_TRACKPOINT_BURST_MAX

ZMK_EVENT_IMPL(zitaotech_trackpoint_dpi_changed);
ZMK_EVENT_IMPL(zitaotech_trackpoint_mirror);

static uint32_t default_dpi_index(void) {
    int32_t index = (DPI_DEFAULT - DPI_MIN) / DPI_STEP;
    if (index < 0) {
        index = 0;
    }
    if (index > (int32_t)(ZITAOTECH_TRACKPOINT_DPI_LEVELS - 1)) {
        index = ZITAOTECH_TRACKPOINT_DPI_LEVELS - 1;
    }
    return (uint32_t)index;
}

static atomic_t dpi_index = ATOMIC_INIT(0); /* set in init, needs default_dpi_index() */
static atomic_t burst_enabled = ATOMIC_INIT(0);
static atomic_t burst_multiplier = ATOMIC_INIT(2);
/* Per-direction fine tuning, in hundredths (100 = x1.00). */
static atomic_t axis_scales[ZITAOTECH_TRACKPOINT_AXIS_COUNT];
/* Linear acceleration ceiling, in hundredths (100 = x1.00 = off). */
static atomic_t accel = ATOMIC_INIT(100);
static atomic_t scroll_speed_percent =
    ATOMIC_INIT(CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_SPEED_DEFAULT);
static atomic_t scroll_burst_enabled = ATOMIC_INIT(0);
static atomic_t scroll_burst_multiplier = ATOMIC_INIT(2);
static atomic_t scroll_accel = ATOMIC_INIT(100);
static atomic_t scroll_key_position = ATOMIC_INIT(CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_KEY_POSITION);
static atomic_t mode_key_positions[ZITAOTECH_TRACKPOINT_MODE_COUNT];
/* Bit per mode, set while that mode key is held (locally or relayed). */
static atomic_t mode_key_pressed_mask = ATOMIC_INIT(0);
static atomic_t arrow_swipe_threshold =
    ATOMIC_INIT(CONFIG_ZITAOTECH_TRACKPOINT_ARROW_SWIPE_THRESHOLD);
static atomic_t scroll_capslock_enabled = ATOMIC_INIT(0);
static atomic_t smooth_scroll_enabled = ATOMIC_INIT(0);
static atomic_t scroll_invert_x = ATOMIC_INIT(0);
static atomic_t scroll_invert_y = ATOMIC_INIT(0);

static inline uint32_t clamp_dpi_index(int32_t index) {
    if (index < 0) {
        return 0;
    }
    if (index > (int32_t)(ZITAOTECH_TRACKPOINT_DPI_LEVELS - 1)) {
        return ZITAOTECH_TRACKPOINT_DPI_LEVELS - 1;
    }
    return (uint32_t)index;
}

uint32_t zitaotech_trackpoint_dpi_index(void) { return (uint32_t)atomic_get(&dpi_index); }

uint32_t zitaotech_trackpoint_dpi_value(void) {
    return DPI_MIN + DPI_STEP * zitaotech_trackpoint_dpi_index();
}

bool zitaotech_trackpoint_burst_enabled(void) { return atomic_get(&burst_enabled) != 0; }

static uint32_t clamp_multiplier(int32_t multiplier) {
    if (multiplier < 1) {
        return 1;
    }
    if (multiplier > BURST_MAX) {
        return BURST_MAX;
    }
    return (uint32_t)multiplier;
}

uint32_t zitaotech_trackpoint_burst_multiplier(void) {
    return clamp_multiplier((int32_t)atomic_get(&burst_multiplier));
}

uint32_t zitaotech_trackpoint_pointer_multiplier(void) {
    return zitaotech_trackpoint_burst_enabled() ? zitaotech_trackpoint_burst_multiplier() : 1;
}

static uint32_t clamp_axis_scale(int32_t scale) {
    if (scale < CONFIG_ZITAOTECH_TRACKPOINT_AXIS_MIN) {
        return CONFIG_ZITAOTECH_TRACKPOINT_AXIS_MIN;
    }
    if (scale > CONFIG_ZITAOTECH_TRACKPOINT_AXIS_MAX) {
        return CONFIG_ZITAOTECH_TRACKPOINT_AXIS_MAX;
    }
    return (uint32_t)scale;
}

uint32_t zitaotech_trackpoint_axis_scale(enum zitaotech_trackpoint_axis axis) {
    if (axis >= ZITAOTECH_TRACKPOINT_AXIS_COUNT) {
        return CONFIG_ZITAOTECH_TRACKPOINT_AXIS_MIN;
    }
    return clamp_axis_scale((int32_t)atomic_get(&axis_scales[axis]));
}

uint32_t zitaotech_trackpoint_accel(void) {
    return (uint32_t)atomic_get(&accel);
}

static uint32_t clamp_scroll_percent(int32_t percent) {
    if (percent < CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_SPEED_MIN) {
        return CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_SPEED_MIN;
    }
    if (percent > CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_SPEED_MAX) {
        return CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_SPEED_MAX;
    }
    return (uint32_t)percent;
}

uint32_t zitaotech_trackpoint_scroll_speed_percent(void) {
    return clamp_scroll_percent((int32_t)atomic_get(&scroll_speed_percent));
}

bool zitaotech_trackpoint_scroll_burst_enabled(void) {
    return atomic_get(&scroll_burst_enabled) != 0;
}

uint32_t zitaotech_trackpoint_scroll_burst_multiplier(void) {
    return clamp_multiplier((int32_t)atomic_get(&scroll_burst_multiplier));
}

uint32_t zitaotech_trackpoint_scroll_multiplier(void) {
    uint32_t multiplier = zitaotech_trackpoint_scroll_speed_percent();
    if (zitaotech_trackpoint_scroll_burst_enabled()) {
        multiplier *= zitaotech_trackpoint_scroll_burst_multiplier();
    }
    return multiplier;
}

uint32_t zitaotech_trackpoint_scroll_accel(void) {
    return (uint32_t)atomic_get(&scroll_accel);
}

int32_t zitaotech_trackpoint_mode_key_position(enum zitaotech_trackpoint_mode mode) {
    if (mode >= ZITAOTECH_TRACKPOINT_MODE_COUNT) {
        return -1;
    }
    if (mode == ZITAOTECH_TRACKPOINT_MODE_SCROLL) {
        return (int32_t)atomic_get(&scroll_key_position);
    }
    return (int32_t)atomic_get(&mode_key_positions[mode]);
}

bool zitaotech_trackpoint_mode_key_pressed(enum zitaotech_trackpoint_mode mode) {
    if (mode >= ZITAOTECH_TRACKPOINT_MODE_COUNT) {
        return false;
    }
    return (atomic_get(&mode_key_pressed_mask) & BIT(mode)) != 0;
}

int32_t zitaotech_trackpoint_scroll_key_position(void) {
    return zitaotech_trackpoint_mode_key_position(ZITAOTECH_TRACKPOINT_MODE_SCROLL);
}

bool zitaotech_trackpoint_scroll_key_pressed(void) {
    return zitaotech_trackpoint_mode_key_pressed(ZITAOTECH_TRACKPOINT_MODE_SCROLL);
}

uint32_t zitaotech_trackpoint_arrow_swipe_threshold(void) {
    uint32_t value = (uint32_t)atomic_get(&arrow_swipe_threshold);
    if (value < CONFIG_ZITAOTECH_TRACKPOINT_ARROW_SWIPE_MIN) {
        return CONFIG_ZITAOTECH_TRACKPOINT_ARROW_SWIPE_MIN;
    }
    if (value > CONFIG_ZITAOTECH_TRACKPOINT_ARROW_SWIPE_MAX) {
        return CONFIG_ZITAOTECH_TRACKPOINT_ARROW_SWIPE_MAX;
    }
    return value;
}

bool zitaotech_trackpoint_scroll_capslock_enabled(void) {
    return atomic_get(&scroll_capslock_enabled) != 0;
}

bool zitaotech_trackpoint_smooth_scroll_enabled(void) { return atomic_get(&smooth_scroll_enabled) != 0; }

bool zitaotech_trackpoint_scroll_invert_x(void) { return atomic_get(&scroll_invert_x) != 0; }

bool zitaotech_trackpoint_scroll_invert_y(void) { return atomic_get(&scroll_invert_y) != 0; }

/*
 * The TrackPoint lives on the peripheral, but the key that enables scrolling
 * can sit on either half.  A key on the peripheral is seen locally; for a key
 * on the central the press/release is relayed across the split link.
 */
ZMK_EVENT_IMPL(zitaotech_trackpoint_scroll_key);
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zitaotech_trackpoint_scroll_key, tsk, source);
ZMK_RELAY_EVENT_HANDLE(zitaotech_trackpoint_scroll_key, tsk, source);

static void raise_scroll_key(uint8_t mask) {
    raise_zitaotech_trackpoint_scroll_key((struct zitaotech_trackpoint_scroll_key){
        .mask = mask,
        .source = ZMK_RELAY_EVENT_SOURCE_SELF,
    });
}

static int scroll_key_position_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t mask = (uint8_t)atomic_get(&mode_key_pressed_mask);
    bool matched = false;
    for (int mode = 0; mode < ZITAOTECH_TRACKPOINT_MODE_COUNT; mode++) {
        int32_t position = zitaotech_trackpoint_mode_key_position(mode);
        if (position < 0 || ev->position != (uint32_t)position) {
            continue;
        }
        matched = true;
        if (ev->state) {
            mask |= BIT(mode);
        } else {
            mask &= (uint8_t)~BIT(mode);
        }
    }

    if (!matched) {
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
    raise_scroll_key(mask);
#else
    atomic_set(&mode_key_pressed_mask, mask);
#endif

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zitaotech_trackpoint_scroll_key_position, scroll_key_position_listener);
ZMK_SUBSCRIPTION(zitaotech_trackpoint_scroll_key_position, zmk_position_state_changed);

static int scroll_key_listener(const zmk_event_t *eh) {
    struct zitaotech_trackpoint_scroll_key *ev = as_zitaotech_trackpoint_scroll_key(eh);
    if (ev == NULL || ev->source == ZMK_RELAY_EVENT_SOURCE_SELF) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    atomic_set(&mode_key_pressed_mask, ev->mask);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zitaotech_trackpoint_scroll_key_apply, scroll_key_listener);
ZMK_SUBSCRIPTION(zitaotech_trackpoint_scroll_key_apply, zitaotech_trackpoint_scroll_key);

static uint32_t clamp_scroll_accel(int32_t value) {
    if (value < CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_ACCEL_MIN) {
        return CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_ACCEL_MIN;
    }
    if (value > CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_ACCEL_MAX) {
        return CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_ACCEL_MAX;
    }
    return (uint32_t)value;
}

/* ---------------------------------------------------------------------------
 * Persistence
 * ------------------------------------------------------------------------ */

static void save_dpi(void) {
    uint16_t value = (uint16_t)zitaotech_trackpoint_dpi_value();
    settings_save_one(SETTINGS_PREFIX "/dpi", &value, sizeof(value));
}

static void save_burst(void) {
    uint8_t enabled = (uint8_t)(zitaotech_trackpoint_burst_enabled() ? 1 : 0);
    uint8_t multiplier = (uint8_t)zitaotech_trackpoint_burst_multiplier();
    settings_save_one(SETTINGS_PREFIX "/burst", &enabled, sizeof(enabled));
    settings_save_one(SETTINGS_PREFIX "/burstmul", &multiplier, sizeof(multiplier));
}

static void save_axis_scales(void) {
    uint8_t values[ZITAOTECH_TRACKPOINT_AXIS_COUNT];
    for (int i = 0; i < ZITAOTECH_TRACKPOINT_AXIS_COUNT; i++) {
        values[i] = (uint8_t)zitaotech_trackpoint_axis_scale(i);
    }
    settings_save_one(SETTINGS_PREFIX "/axis", values, sizeof(values));
}

static void save_accel(void) {
    uint16_t value = (uint16_t)zitaotech_trackpoint_accel();
    settings_save_one(SETTINGS_PREFIX "/accel", &value, sizeof(value));
}

static void save_scroll(void) {
    uint16_t speed = (uint16_t)zitaotech_trackpoint_scroll_speed_percent();
    uint8_t enabled = (uint8_t)(zitaotech_trackpoint_scroll_burst_enabled() ? 1 : 0);
    uint8_t multiplier = (uint8_t)zitaotech_trackpoint_scroll_burst_multiplier();
    uint16_t accel_value = (uint16_t)zitaotech_trackpoint_scroll_accel();
    settings_save_one(SETTINGS_PREFIX "/scroll", &speed, sizeof(speed));
    settings_save_one(SETTINGS_PREFIX "/sburst", &enabled, sizeof(enabled));
    settings_save_one(SETTINGS_PREFIX "/sburstmul", &multiplier, sizeof(multiplier));
    settings_save_one(SETTINGS_PREFIX "/saccel", &accel_value, sizeof(accel_value));
}

static void save_scroll_key(void) {
    int32_t position = zitaotech_trackpoint_scroll_key_position();
    settings_save_one(SETTINGS_PREFIX "/skey", &position, sizeof(position));
}

static void save_scroll_options(void) {
    uint8_t smooth = zitaotech_trackpoint_smooth_scroll_enabled() ? 1 : 0;
    settings_save_one(SETTINGS_PREFIX "/smooth", &smooth, sizeof(smooth));
    uint8_t capslock = (uint8_t)(zitaotech_trackpoint_scroll_capslock_enabled() ? 1 : 0);
    uint8_t invert_x = (uint8_t)(zitaotech_trackpoint_scroll_invert_x() ? 1 : 0);
    uint8_t invert_y = (uint8_t)(zitaotech_trackpoint_scroll_invert_y() ? 1 : 0);
    settings_save_one(SETTINGS_PREFIX "/capscroll", &capslock, sizeof(capslock));
    settings_save_one(SETTINGS_PREFIX "/sinvx", &invert_x, sizeof(invert_x));
    settings_save_one(SETTINGS_PREFIX "/sinvy", &invert_y, sizeof(invert_y));
}

static void save_mode_keys(void) {
    for (int mode = 0; mode < ZITAOTECH_TRACKPOINT_MODE_COUNT; mode++) {
        int32_t position = zitaotech_trackpoint_mode_key_position(mode);
        char path[64];
        snprintf(path, sizeof(path), SETTINGS_PREFIX "/mode%d", mode);
        settings_save_one(path, &position, sizeof(position));
    }
    uint16_t threshold = (uint16_t)zitaotech_trackpoint_arrow_swipe_threshold();
    settings_save_one(SETTINGS_PREFIX "/arrowthr", &threshold, sizeof(threshold));
}

static void persist_all(void) {
    save_dpi();
    save_burst();
    save_axis_scales();
    save_accel();
    save_scroll();
    save_scroll_key();
    save_scroll_options();
    save_mode_keys();
}

/*
 * Flash writes are debounced: turning the DPI up five steps with the &DPI keys
 * should not mean five flash writes, and the key path should never block on
 * one.
 */
static void persist_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    persist_all();
}

static K_WORK_DELAYABLE_DEFINE(trackpoint_persist_work, persist_work_cb);

int zitaotech_trackpoint_save(void) {
    persist_all();
    return 0;
}

__weak void zitaotech_trackpoint_state_changed(void) {}

static atomic_t notify_suppressed = ATOMIC_INIT(0);

void zitaotech_trackpoint_suppress_notifications(bool suppress) {
    atomic_set(&notify_suppressed, suppress ? 1 : 0);
}

/* ---------------------------------------------------------------------------
 * Relay: keep both halves' copies in step
 * ------------------------------------------------------------------------ */

/*
 * The relay macros only forward events raised locally (source == self), so a
 * relayed value applied below is never echoed back.
 */
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zitaotech_trackpoint_mirror, tpm, source);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zitaotech_trackpoint_mirror, tpm, source);
ZMK_RELAY_EVENT_HANDLE(zitaotech_trackpoint_mirror, tpm, source);

static void raise_mirror(void) {
    raise_zitaotech_trackpoint_mirror((struct zitaotech_trackpoint_mirror){
        .dpi_index = (uint8_t)zitaotech_trackpoint_dpi_index(),
        .burst_enabled = (uint8_t)(zitaotech_trackpoint_burst_enabled() ? 1 : 0),
        .burst_multiplier = (uint8_t)zitaotech_trackpoint_burst_multiplier(),
        .smooth_scroll_enabled = zitaotech_trackpoint_smooth_scroll_enabled() ? 1 : 0,
        .capslock_scroll_enabled = zitaotech_trackpoint_scroll_capslock_enabled() ? 1 : 0,
        .arrow_threshold = (uint16_t)zitaotech_trackpoint_arrow_swipe_threshold(),
        .source = ZMK_RELAY_EVENT_SOURCE_SELF,
    });
}

static void raise_dpi_changed(void) {
    raise_zitaotech_trackpoint_dpi_changed((struct zitaotech_trackpoint_dpi_changed){
        .dpi_value = zitaotech_trackpoint_dpi_value(),
        .dpi_index = zitaotech_trackpoint_dpi_index(),
    });
}

static void mark_changed(void) {
    k_work_reschedule(&trackpoint_persist_work,
                      K_MSEC(CONFIG_ZITAOTECH_TRACKPOINT_SAVE_DEBOUNCE_MS));
    if (!atomic_get(&notify_suppressed)) {
        zitaotech_trackpoint_state_changed();
    }
}

static int mirror_listener(const zmk_event_t *eh) {
    struct zitaotech_trackpoint_mirror *ev = as_zitaotech_trackpoint_mirror(eh);

    if (ev == NULL || ev->source == ZMK_RELAY_EVENT_SOURCE_SELF) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint32_t index = clamp_dpi_index((int32_t)ev->dpi_index);
    bool dpi_changed = index != zitaotech_trackpoint_dpi_index();

    atomic_set(&dpi_index, index);
    atomic_set(&burst_enabled, ev->burst_enabled ? 1 : 0);
    atomic_set(&smooth_scroll_enabled, ev->smooth_scroll_enabled ? 1 : 0);
    atomic_set(&scroll_capslock_enabled, ev->capslock_scroll_enabled ? 1 : 0);
    atomic_set(&burst_multiplier, (atomic_val_t)clamp_multiplier(ev->burst_multiplier));
    /* The arrow threshold is only *used* by the half that reads the
     * TrackPoint, so it has to arrive with everything else. */
    atomic_set(&arrow_swipe_threshold,
               (atomic_val_t)MAX((uint32_t)CONFIG_ZITAOTECH_TRACKPOINT_ARROW_SWIPE_MIN,
                                 (uint32_t)ev->arrow_threshold));

    LOG_DBG("Mirrored TrackPoint state: dpi index %u, burst %u x%u", index, ev->burst_enabled,
            ev->burst_multiplier);

    if (dpi_changed) {
        raise_dpi_changed();
    }
    mark_changed();

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zitaotech_trackpoint_mirror_apply, mirror_listener);
ZMK_SUBSCRIPTION(zitaotech_trackpoint_mirror_apply, zitaotech_trackpoint_mirror);

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */

int zitaotech_trackpoint_set_dpi_index(uint32_t index) {
    uint32_t clamped = clamp_dpi_index((int32_t)index);
    uint32_t previous = zitaotech_trackpoint_dpi_index();

    if (clamped != previous) {
        atomic_set(&dpi_index, clamped);
        raise_dpi_changed();
    }

    raise_mirror();
    mark_changed();

    return (int)clamped;
}

int zitaotech_trackpoint_step_dpi(int delta) {
    return zitaotech_trackpoint_set_dpi_index((uint32_t)((int)zitaotech_trackpoint_dpi_index() + delta * (10 / DPI_STEP)));
}

int zitaotech_trackpoint_set_burst(bool enabled, uint32_t multiplier) {
    atomic_set(&burst_enabled, enabled ? 1 : 0);
    atomic_set(&burst_multiplier, (atomic_val_t)clamp_multiplier((int32_t)multiplier));

    raise_mirror();
    mark_changed();

    return 0;
}

int zitaotech_trackpoint_set_axis_scales(
    const uint32_t scales[ZITAOTECH_TRACKPOINT_AXIS_COUNT]) {
    if (scales == NULL) {
        return -EINVAL;
    }

    for (int i = 0; i < ZITAOTECH_TRACKPOINT_AXIS_COUNT; i++) {
        uint32_t clamped = clamp_axis_scale((int32_t)scales[i]);
        if (scales[i] < CONFIG_ZITAOTECH_TRACKPOINT_AXIS_MIN ||
            scales[i] > CONFIG_ZITAOTECH_TRACKPOINT_AXIS_MAX) {
            return -ERANGE;
        }
        atomic_set(&axis_scales[i], (atomic_val_t)clamped);
    }

    mark_changed();
    return 0;
}

int zitaotech_trackpoint_reset_axis_scales(void) {
    for (int i = 0; i < ZITAOTECH_TRACKPOINT_AXIS_COUNT; i++) {
        atomic_set(&axis_scales[i], CONFIG_ZITAOTECH_TRACKPOINT_AXIS_MIN);
    }
    mark_changed();
    return 0;
}

int zitaotech_trackpoint_set_accel(uint32_t value) {
    if (value < CONFIG_ZITAOTECH_TRACKPOINT_ACCEL_MIN ||
        value > CONFIG_ZITAOTECH_TRACKPOINT_ACCEL_MAX) {
        return -ERANGE;
    }

    atomic_set(&accel, (atomic_val_t)value);
    mark_changed();
    return 0;
}

int zitaotech_trackpoint_set_scroll(uint32_t speed_percent, bool burst_enabled,
                                    uint32_t burst_multiplier, uint32_t accel_value) {
    if (speed_percent < CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_SPEED_MIN ||
        speed_percent > CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_SPEED_MAX ||
        burst_multiplier < 1 || burst_multiplier > CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_BURST_MAX ||
        accel_value < CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_ACCEL_MIN ||
        accel_value > CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_ACCEL_MAX) {
        return -ERANGE;
    }

    atomic_set(&scroll_speed_percent, (atomic_val_t)speed_percent);
    atomic_set(&scroll_burst_enabled, burst_enabled ? 1 : 0);
    atomic_set(&scroll_burst_multiplier, (atomic_val_t)burst_multiplier);
    atomic_set(&scroll_accel, (atomic_val_t)accel_value);
    mark_changed();
    return 0;
}

int zitaotech_trackpoint_set_scroll_key_position(int32_t position) {
    if (position < -1) {
        return -EINVAL;
    }

    atomic_set(&scroll_key_position, (atomic_val_t)position);
    if (position < 0) {
        atomic_and(&mode_key_pressed_mask,
                   (atomic_val_t)~(uint32_t)BIT(ZITAOTECH_TRACKPOINT_MODE_SCROLL));
    }
    mark_changed();
    return 0;
}

int zitaotech_trackpoint_set_mode_key(enum zitaotech_trackpoint_mode mode, int32_t position) {
    if (mode >= ZITAOTECH_TRACKPOINT_MODE_COUNT || position < -1) {
        return -EINVAL;
    }

    if (mode == ZITAOTECH_TRACKPOINT_MODE_SCROLL) {
        return zitaotech_trackpoint_set_scroll_key_position(position);
    }

    atomic_set(&mode_key_positions[mode], (atomic_val_t)position);
    if (position < 0) {
        atomic_and(&mode_key_pressed_mask, (atomic_val_t)~(uint32_t)BIT(mode));
    }
    mark_changed();
    return 0;
}

int zitaotech_trackpoint_set_arrow_swipe_threshold(uint32_t threshold) {
    if (threshold < CONFIG_ZITAOTECH_TRACKPOINT_ARROW_SWIPE_MIN ||
        threshold > CONFIG_ZITAOTECH_TRACKPOINT_ARROW_SWIPE_MAX) {
        return -ERANGE;
    }
    atomic_set(&arrow_swipe_threshold, (atomic_val_t)threshold);
    /* Mirror it: the arrow-mode gesture is evaluated on the peripheral. */
    raise_mirror();
    mark_changed();
    return 0;
}

int zitaotech_trackpoint_set_scroll_options(bool capslock_scroll, bool invert_x, bool invert_y) {
    atomic_set(&scroll_capslock_enabled, capslock_scroll ? 1 : 0);
    atomic_set(&scroll_invert_x, invert_x ? 1 : 0);
    atomic_set(&scroll_invert_y, invert_y ? 1 : 0);
    raise_mirror();
    mark_changed();
    return 0;
}

int zitaotech_trackpoint_set_smooth_scroll(bool enabled) {
    atomic_set(&smooth_scroll_enabled, enabled ? 1 : 0);
    raise_mirror();
    mark_changed();
    return 0;
}

int zitaotech_trackpoint_reset(void) {
    atomic_set(&scroll_capslock_enabled, 0);
    atomic_set(&scroll_invert_x, 0);
    atomic_set(&scroll_invert_y, 0);
    atomic_set(&smooth_scroll_enabled, 0);
    atomic_set(&dpi_index, default_dpi_index());
    atomic_set(&burst_enabled, 0);
    atomic_set(&burst_multiplier, 2);
    atomic_set(&accel, CONFIG_ZITAOTECH_TRACKPOINT_ACCEL_MIN);
    atomic_set(&scroll_speed_percent, CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_SPEED_DEFAULT);
    atomic_set(&scroll_burst_enabled, 0);
    atomic_set(&scroll_burst_multiplier, 2);
    atomic_set(&scroll_accel, CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_ACCEL_MIN);
    atomic_set(&scroll_key_position, CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_KEY_POSITION);
    atomic_set(&mode_key_pressed_mask, 0);
    for (int mode = 0; mode < ZITAOTECH_TRACKPOINT_MODE_COUNT; mode++) {
        atomic_set(&mode_key_positions[mode], -1);
    }
    atomic_set(&arrow_swipe_threshold, CONFIG_ZITAOTECH_TRACKPOINT_ARROW_SWIPE_THRESHOLD);
    zitaotech_trackpoint_reset_axis_scales();

    raise_dpi_changed();
    raise_mirror();
    mark_changed();

    return 0;
}

/* ---------------------------------------------------------------------------
 * Settings
 * ------------------------------------------------------------------------ */

static int trackpoint_settings_set(const char *name, size_t len, settings_read_cb read_cb,
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
        atomic_set(&burst_multiplier, (atomic_val_t)clamp_multiplier((int32_t)value));
        return 0;
    }

    if (settings_name_steq(name, "axis", &next) && !next) {
        if (len != sizeof(uint8_t) * ZITAOTECH_TRACKPOINT_AXIS_COUNT) {
            return -EINVAL;
        }
        uint8_t values[ZITAOTECH_TRACKPOINT_AXIS_COUNT];
        int rc = read_cb(cb_arg, values, sizeof(values));
        if (rc < 0) {
            return rc;
        }
        for (int i = 0; i < ZITAOTECH_TRACKPOINT_AXIS_COUNT; i++) {
            atomic_set(&axis_scales[i], (atomic_val_t)clamp_axis_scale((int32_t)values[i]));
        }
        return 0;
    }

    if (settings_name_steq(name, "accel", &next) && !next) {
        if (len != sizeof(uint16_t)) {
            return -EINVAL;
        }
        uint16_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        if (value < CONFIG_ZITAOTECH_TRACKPOINT_ACCEL_MIN) {
            value = CONFIG_ZITAOTECH_TRACKPOINT_ACCEL_MIN;
        }
        if (value > CONFIG_ZITAOTECH_TRACKPOINT_ACCEL_MAX) {
            value = CONFIG_ZITAOTECH_TRACKPOINT_ACCEL_MAX;
        }
        atomic_set(&accel, (atomic_val_t)value);
        return 0;
    }

    if (settings_name_steq(name, "scroll", &next) && !next) {
        if (len != sizeof(uint16_t)) {
            return -EINVAL;
        }
        uint16_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        atomic_set(&scroll_speed_percent, (atomic_val_t)clamp_scroll_percent((int32_t)value));
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
        atomic_set(&scroll_burst_multiplier, (atomic_val_t)clamp_multiplier((int32_t)value));
        return 0;
    }

    if (settings_name_steq(name, "saccel", &next) && !next) {
        if (len != sizeof(uint16_t)) {
            return -EINVAL;
        }
        uint16_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        atomic_set(&scroll_accel, (atomic_val_t)clamp_scroll_accel((int32_t)value));
        return 0;
    }

    if (settings_name_steq(name, "skey", &next) && !next) {
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
        atomic_set(&scroll_key_position, (atomic_val_t)value);
        return 0;
    }

    if (settings_name_steq(name, "smooth", &next) && !next) {
        if (len != sizeof(uint8_t)) { return -EINVAL; }
        uint8_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) { return rc; }
        atomic_set(&smooth_scroll_enabled, value ? 1 : 0);
        return 0;
    }

    if (settings_name_steq(name, "capscroll", &next) && !next) {
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }
        uint8_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        atomic_set(&scroll_capslock_enabled, value ? 1 : 0);
        return 0;
    }

    if (settings_name_steq(name, "sinvx", &next) && !next) {
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }
        uint8_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        atomic_set(&scroll_invert_x, value ? 1 : 0);
        return 0;
    }

    if (settings_name_steq(name, "sinvy", &next) && !next) {
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }
        uint8_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        atomic_set(&scroll_invert_y, value ? 1 : 0);
        return 0;
    }

    for (int mode = 0; mode < ZITAOTECH_TRACKPOINT_MODE_COUNT; mode++) {
        char key[8];
        snprintf(key, sizeof(key), "mode%d", mode);
        if (settings_name_steq(name, key, &next) && !next) {
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
            if (mode == ZITAOTECH_TRACKPOINT_MODE_SCROLL) {
                atomic_set(&scroll_key_position, (atomic_val_t)value);
            } else {
                atomic_set(&mode_key_positions[mode], (atomic_val_t)value);
            }
            return 0;
        }
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
        atomic_set(&arrow_swipe_threshold, (atomic_val_t)value);
        return 0;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(zitaotech_trackpoint, SETTINGS_PREFIX, NULL,
                               trackpoint_settings_set, NULL, NULL);

static int zitaotech_trackpoint_init(void) {
    atomic_set(&dpi_index, default_dpi_index());
    atomic_set(&burst_multiplier, (atomic_val_t)clamp_multiplier(2));
    for (int mode = 0; mode < ZITAOTECH_TRACKPOINT_MODE_COUNT; mode++) {
        atomic_set(&mode_key_positions[mode], -1);
    }
    atomic_set(&arrow_swipe_threshold, CONFIG_ZITAOTECH_TRACKPOINT_ARROW_SWIPE_THRESHOLD);
    for (int i = 0; i < ZITAOTECH_TRACKPOINT_AXIS_COUNT; i++) {
        atomic_set(&axis_scales[i], CONFIG_ZITAOTECH_TRACKPOINT_AXIS_MIN);
    }
    atomic_set(&accel, CONFIG_ZITAOTECH_TRACKPOINT_ACCEL_MIN);
    atomic_set(&scroll_burst_multiplier, (atomic_val_t)clamp_multiplier(2));
    atomic_set(&scroll_accel, CONFIG_ZITAOTECH_TRACKPOINT_SCROLL_ACCEL_MIN);

    LOG_INF("trackpoint control: dpi=%u (%u..%u step %u, %u levels) burst=%s",
            (unsigned int)zitaotech_trackpoint_dpi_value(), DPI_MIN, DPI_MAX, DPI_STEP,
            (unsigned int)ZITAOTECH_TRACKPOINT_DPI_LEVELS,
            zitaotech_trackpoint_burst_enabled() ? "on" : "off");

    return 0;
}

SYS_INIT(zitaotech_trackpoint_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
