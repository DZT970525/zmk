/*
 * Copyright (c) 2023 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <zmk/indicator.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_indicator),
             "CONFIG_ZMK_INDICATOR is enabled but no zmk,indicator chosen node found");

static const struct device *const indicator_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_indicator));

#define CHILD_COUNT(...) +1
#define DT_NUM_CHILD(node_id) (DT_FOREACH_CHILD(node_id, CHILD_COUNT))

#define INDICATOR_NUM_LEDS (DT_NUM_CHILD(DT_CHOSEN(zmk_indicator)))

#define BRT_MAX 100

struct indicator_state {
    uint8_t brightness;
    bool on;
};

static struct indicator_state state = {.brightness = CONFIG_ZMK_INDICATOR_BRT_START,
                                       .on = IS_ENABLED(CONFIG_ZMK_INDICATOR_ON_START)};

static int zmk_indicator_update() {
    uint8_t brt = zmk_indicator_get_brt();
    LOG_DBG("Update indicator brightness: %d%%", brt);

    for (int i = 0; i < INDICATOR_NUM_LEDS; i++) {
        int rc = led_set_brightness(indicator_dev, i, brt);
        if (rc != 0) {
            LOG_ERR("Failed to update indicator LED %d: %d", i, rc);
            return rc;
        }
    }
    return 0;
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int indicator_settings_load_cb(const char *name, size_t len, settings_read_cb read_cb,
                                      void *cb_arg, void *param) {
    const char *next;
    if (settings_name_steq(name, "state", &next) && !next) {
        if (len != sizeof(state)) {
            return -EINVAL;
        }

        int rc = read_cb(cb_arg, &state, sizeof(state));
        return MIN(rc, 0);
    }
    return -ENOENT;
}

static void indicator_save_work_handler(struct k_work *work) {
    settings_save_one("indicator/state", &state, sizeof(state));
}

static struct k_work_delayable indicator_save_work;
#endif

static int zmk_indicator_init(void) {
    if (!device_is_ready(indicator_dev)) {
        LOG_ERR("INDICATOR device \"%s\" is not ready", indicator_dev->name);
        return -ENODEV;
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_subsys_init();
    int rc = settings_load_subtree_direct("indicator", indicator_settings_load_cb, NULL);
    if (rc != 0) {
        LOG_ERR("Failed to load indicator settings: %d", rc);
    }
    k_work_init_delayable(&indicator_save_work, indicator_save_work_handler);
#endif
#if IS_ENABLED(CONFIG_ZMK_INDICATOR_AUTO_OFF_USB)
    state.on = zmk_usb_is_powered();
#endif
    return zmk_indicator_update();
}

static int zmk_indicator_update_and_save() {
    int rc = zmk_indicator_update();
    if (rc != 0) {
        return rc;
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    int ret = k_work_reschedule(&indicator_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    return MIN(ret, 0);
#else
    return 0;
#endif
}

int zmk_indicator_on() {
    state.brightness = MAX(state.brightness, CONFIG_ZMK_INDICATOR_BRT_STEP);
    state.on = true;
    return zmk_indicator_update_and_save();
}

int zmk_indicator_off() {
    state.on = false;
    return zmk_indicator_update_and_save();
}

int zmk_indicator_toggle() { return state.on ? zmk_indicator_off() : zmk_indicator_on(); }

bool zmk_indicator_is_on() { return state.on; }

int zmk_indicator_set_brt(uint8_t brightness) {
    state.brightness = MIN(brightness, BRT_MAX);
    state.on = (state.brightness > 0);
    return zmk_indicator_update_and_save();
}

uint8_t zmk_indicator_get_brt() { return state.on ? state.brightness : 0; }

uint8_t zmk_indicator_calc_brt(int direction) {
    int brt = state.brightness + (direction * CONFIG_ZMK_INDICATOR_BRT_STEP);
    return CLAMP(brt, 0, BRT_MAX);
}

uint8_t zmk_indicator_calc_brt_cycle() {
    if (state.brightness == BRT_MAX) {
        return 0;
    } else {
        return zmk_indicator_calc_brt(1);
    }
}

#if IS_ENABLED(CONFIG_ZMK_INDICATOR_AUTO_OFF_IDLE) || IS_ENABLED(CONFIG_ZMK_INDICATOR_AUTO_OFF_USB)
static int indicator_auto_state(bool *prev_state, bool new_state) {
    if (state.on == new_state) {
        return 0;
    }
    state.on = new_state && *prev_state;
    *prev_state = !new_state;
    return zmk_indicator_update();
}

static int indicator_event_listener(const zmk_event_t *eh) {

#if IS_ENABLED(CONFIG_ZMK_INDICATOR_AUTO_OFF_IDLE)
    if (as_zmk_activity_state_changed(eh)) {
        static bool prev_state = false;
        return indicator_auto_state(&prev_state, zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_INDICATOR_AUTO_OFF_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        static bool prev_state = false;
        return indicator_auto_state(&prev_state, zmk_usb_is_powered());
    }
#endif

    return -ENOTSUP;
}

ZMK_LISTENER(indicator, indicator_event_listener);
#endif // IS_ENABLED(CONFIG_ZMK_INDICATOR_AUTO_OFF_IDLE) ||
       // IS_ENABLED(CONFIG_ZMK_INDICATOR_AUTO_OFF_USB)

#if IS_ENABLED(CONFIG_ZMK_INDICATOR_AUTO_OFF_IDLE)
ZMK_SUBSCRIPTION(indicator, zmk_activity_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_INDICATOR_AUTO_OFF_USB)
ZMK_SUBSCRIPTION(indicator, zmk_usb_conn_state_changed);
#endif

SYS_INIT(zmk_indicator_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);