/*
 * Runtime backlight settings for ZitaoTech boards.
 *
 * Brightness and on/off are ZMK's own backlight state (which is already
 * persisted); this module adds the per-layer effect of the layer indicator LED
 * and exposes both to ZitaoTech Studio.  The board's keyboard_backlight.c asks
 * this module which effect a layer should show.
 *
 * Copyright (c) 2026 ZitaoTech
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zmk/backlight.h>
#include <zmk/activity.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(zitaotech_backlight, CONFIG_ZMK_LOG_LEVEL);

#include <zitaotech/zmk/backlight.h>

#define SETTINGS_PREFIX "ztb"

#define LAYER_COUNT CONFIG_ZITAOTECH_BACKLIGHT_LAYER_COUNT

BUILD_ASSERT(LAYER_COUNT <= 4, "Only layers 1..4 have an effect setting");

/* Factory defaults: layer 1 blinks, layer 2 breathes, layer 3 blinks twice as
 * fast - the behaviour these boards shipped with - and layer 4 (the automatic
 * mouse layer) follows the backlight brightness. */
static const uint8_t default_effects[4] = {
    ZITAOTECH_BACKLIGHT_EFFECT_BLINK,
    ZITAOTECH_BACKLIGHT_EFFECT_BREATH,
    ZITAOTECH_BACKLIGHT_EFFECT_FAST_BLINK,
    ZITAOTECH_BACKLIGHT_EFFECT_BACKLIGHT,
};

static atomic_t layer_effects[LAYER_COUNT];
static atomic_t notify_suppressed = ATOMIC_INIT(0);

/*
 * The user's choice ("is the backlight switched on?"), which is what Studio
 * shows on its switch.  ZMK's own state is not usable for that: the idle
 * auto-off turns it off after the idle timeout, and the switch in the UI must
 * not follow that.  ZMK restores this choice when the keyboard wakes again
 * (see zmk_backlight_sync_auto_state_choice).
 */
static atomic_t logical_on = ATOMIC_INIT(1);

/* Last values pushed to Studio, so the poll below only notifies on change. */
static bool last_on;
static uint8_t last_brightness;

static const char *const effect_setting_names[4] = {"l1", "l2", "l3", "l4"};

static void load_defaults(void) {
    for (uint32_t i = 0; i < LAYER_COUNT; i++) {
        atomic_set(&layer_effects[i], (atomic_val_t)default_effects[i]);
    }
}

uint32_t zitaotech_backlight_layer_count(void) { return LAYER_COUNT; }

uint8_t zitaotech_backlight_layer_effect(uint32_t layer) {
    if (layer < 1 || layer > LAYER_COUNT) {
        /* Layer 0 and anything above the configurable range (the automatic
         * mouse layer, for one) simply follow the backlight. */
        return ZITAOTECH_BACKLIGHT_EFFECT_BACKLIGHT;
    }
    return (uint8_t)atomic_get(&layer_effects[layer - 1]);
}

static void save_layer_effect(uint32_t layer) {
    char path[24];
    uint8_t value = zitaotech_backlight_layer_effect(layer);

    snprintf(path, sizeof(path), SETTINGS_PREFIX "/%s", effect_setting_names[layer - 1]);
    settings_save_one(path, &value, sizeof(value));
}

int zitaotech_backlight_set_layer_effect(uint32_t layer, uint8_t effect) {
    if (layer < 1 || layer > LAYER_COUNT) {
        return -EINVAL;
    }
    if (effect >= ZITAOTECH_BACKLIGHT_EFFECT_COUNT) {
        return -EINVAL;
    }
    atomic_set(&layer_effects[layer - 1], (atomic_val_t)effect);
    save_layer_effect(layer);
    if (!atomic_get(&notify_suppressed)) {
        zitaotech_backlight_state_changed();
    }
    return 0;
}

bool zitaotech_backlight_is_on(void) { return atomic_get(&logical_on) != 0; }

uint8_t zitaotech_backlight_brightness(void) { return zmk_backlight_get_brt(); }

uint8_t zitaotech_backlight_brightness_min(void) { return CONFIG_ZITAOTECH_BACKLIGHT_BRT_MIN; }

uint8_t zitaotech_backlight_brightness_max(void) { return CONFIG_ZITAOTECH_BACKLIGHT_BRT_MAX; }

int zitaotech_backlight_set_on(bool on) {
    /*
     * NOTE: this fork's activity API has no zmk_activity_wake(); the backlight
     * comes back on with the next key press or trackpad movement.
     */
    int rc = on ? zmk_backlight_on() : zmk_backlight_off();
    if (rc < 0) {
        return rc;
    }
    atomic_set(&logical_on, on ? 1 : 0);
    last_on = on;
    last_brightness = zmk_backlight_get_brt();
    if (!atomic_get(&notify_suppressed)) {
        zitaotech_backlight_state_changed();
    }
    return 0;
}

int zitaotech_backlight_set_brightness(uint8_t brightness) {
    if (brightness < CONFIG_ZITAOTECH_BACKLIGHT_BRT_MIN ||
        brightness > CONFIG_ZITAOTECH_BACKLIGHT_BRT_MAX) {
        return -EINVAL;
    }
    /* The new brightness is visible again from the next activity onwards. */
    int rc = zmk_backlight_set_brt(brightness);
    if (rc < 0) {
        return rc;
    }
    last_brightness = brightness;
    last_on = zitaotech_backlight_is_on();
    if (!atomic_get(&notify_suppressed)) {
        zitaotech_backlight_state_changed();
    }
    return 0;
}

/*
 * Brightness can also change from the keyboard itself (&bl keys, and the layer
 * 2 C / N keys), which is exactly where the web UI has to follow along live.
 * ZMK does not raise an event for that, so watch its state instead.
 */
#define POLL_INTERVAL_MS 100

static struct k_work_delayable poll_work;

static void poll_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    bool active = (zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    bool logical = atomic_get(&logical_on) != 0;
    uint8_t brightness = zmk_backlight_get_brt();

    /*
     * While the keyboard is awake its backlight state is exactly the user's
     * choice - the &bl keys included - so follow it.  While it is idle ZMK's
     * auto-off has the backlight dark, which must not flip the Studio switch.
     */
    if (active) {
        bool on = zmk_backlight_is_on();
        if (on != logical) {
            logical = on;
            atomic_set(&logical_on, on ? 1 : 0);
        }
    }

    if (logical != last_on || brightness != last_brightness) {
        last_on = logical;
        last_brightness = brightness;
        if (!atomic_get(&notify_suppressed)) {
            zitaotech_backlight_state_changed();
        }
    }

    k_work_reschedule(&poll_work, K_MSEC(POLL_INTERVAL_MS));
}

__weak void zitaotech_backlight_state_changed(void) {}

void zitaotech_backlight_suppress_notifications(bool suppress) {
    atomic_set(&notify_suppressed, suppress ? 1 : 0);
}

static int backlight_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    const char *next;

    for (uint32_t i = 0; i < LAYER_COUNT; i++) {
        if (!settings_name_steq(name, effect_setting_names[i], &next) || next) {
            continue;
        }
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }
        uint8_t value;
        int rc = read_cb(cb_arg, &value, sizeof(value));
        if (rc < 0) {
            return rc;
        }
        if (value >= ZITAOTECH_BACKLIGHT_EFFECT_COUNT) {
            value = default_effects[i];
        }
        atomic_set(&layer_effects[i], (atomic_val_t)value);
        return 0;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(zitaotech_backlight, SETTINGS_PREFIX, NULL, backlight_settings_set,
                               NULL, NULL);

static int zitaotech_backlight_init(void) {
    load_defaults();
    atomic_set(&logical_on, zmk_backlight_is_on() ? 1 : 0);
    last_on = zmk_backlight_is_on();
    last_brightness = zmk_backlight_get_brt();
    k_work_init_delayable(&poll_work, poll_work_handler);
    k_work_reschedule(&poll_work, K_MSEC(POLL_INTERVAL_MS));
    LOG_INF("backlight control: on=%d brt=%u (layers %u)", last_on, last_brightness,
            (unsigned int)LAYER_COUNT);
    return 0;
}

SYS_INIT(zitaotech_backlight_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
