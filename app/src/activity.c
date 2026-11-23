/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>

#include <zmk/pm.h>

#include <zmk/activity.h>

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_POINTING)
#include <zephyr/input/input.h>
#endif

bool is_usb_power_present(void) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    return zmk_usb_is_powered();
#else
    return false;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
}

static enum zmk_activity_state activity_state;

static int64_t activity_last_uptime;

/*
 * The inactive timeouts are runtime values (persisted through the settings
 * subsystem) instead of compile time constants so that ZitaoTech Studio can
 * change them.  0 means "never time out".
 */
static uint32_t max_idle_ms = CONFIG_ZMK_IDLE_TIMEOUT;

#if IS_ENABLED(CONFIG_ZMK_SLEEP)
static uint32_t max_sleep_ms = CONFIG_ZMK_IDLE_SLEEP_TIMEOUT;
#endif

/*
 * The values are written from the settings/RPC thread and read from the system
 * work queue, so keep the reads/writes locked.  This is a leaf lock: nothing
 * inside the critical sections can block.
 */
static struct k_spinlock activity_timeout_lock;

uint32_t zmk_activity_get_idle_ms(void) {
    k_spinlock_key_t key = k_spin_lock(&activity_timeout_lock);
    uint32_t ret = max_idle_ms;
    k_spin_unlock(&activity_timeout_lock, key);
    return ret;
}

uint32_t zmk_activity_get_sleep_ms(void) {
#if IS_ENABLED(CONFIG_ZMK_SLEEP)
    k_spinlock_key_t key = k_spin_lock(&activity_timeout_lock);
    uint32_t ret = max_sleep_ms;
    k_spin_unlock(&activity_timeout_lock, key);
    return ret;
#else
    return 0;
#endif
}

#if IS_ENABLED(CONFIG_SETTINGS)

static void activity_save_timeouts_work(struct k_work *work) {
    uint32_t idle = zmk_activity_get_idle_ms();
    uint32_t sleep = zmk_activity_get_sleep_ms();

    settings_save_one("activity/idle_ms", &idle, sizeof(idle));
#if IS_ENABLED(CONFIG_ZMK_SLEEP)
    settings_save_one("activity/sleep_ms", &sleep, sizeof(sleep));
#endif
}

static K_WORK_DELAYABLE_DEFINE(activity_save_timeouts, activity_save_timeouts_work);

static int activity_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                 void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "idle_ms", &next) && !next) {
        if (len != sizeof(max_idle_ms)) {
            return -EINVAL;
        }
        return read_cb(cb_arg, &max_idle_ms, sizeof(max_idle_ms)) < 0 ? -EINVAL : 0;
    }

#if IS_ENABLED(CONFIG_ZMK_SLEEP)
    if (settings_name_steq(name, "sleep_ms", &next) && !next) {
        if (len != sizeof(max_sleep_ms)) {
            return -EINVAL;
        }
        return read_cb(cb_arg, &max_sleep_ms, sizeof(max_sleep_ms)) < 0 ? -EINVAL : 0;
    }
#endif

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(activity, "activity", NULL, activity_settings_set, NULL, NULL);

#endif // IS_ENABLED(CONFIG_SETTINGS)

bool zmk_activity_set_idle_ms(uint32_t idle_ms) {
#if IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q10) || IS_ENABLED(CONFIG_BOARD_BBP9981) || \
    IS_ENABLED(CONFIG_BOARD_ZITAOTECH_Q20) || IS_ENABLED(CONFIG_BOARD_P9981)
    /* Studio exposes whole seconds, with zero meaning always on. */
    if (idle_ms != 0 && (idle_ms < 1000 || idle_ms > 6000000 || idle_ms % 1000 != 0)) {
        return false;
    }
#endif
    k_spinlock_key_t key = k_spin_lock(&activity_timeout_lock);
    max_idle_ms = idle_ms;
    k_spin_unlock(&activity_timeout_lock, key);

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_reschedule(&activity_save_timeouts, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
#endif

    LOG_INF("Idle timeout is now %u ms", idle_ms);
    return true;
}

bool zmk_activity_set_sleep_ms(uint32_t sleep_ms) {
#if IS_ENABLED(CONFIG_ZMK_SLEEP)
    k_spinlock_key_t key = k_spin_lock(&activity_timeout_lock);
    max_sleep_ms = sleep_ms;
    k_spin_unlock(&activity_timeout_lock, key);

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_reschedule(&activity_save_timeouts, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
#endif

    LOG_INF("Sleep timeout is now %u ms", sleep_ms);
#else
    ARG_UNUSED(sleep_ms);
#endif

    return true;
}

int raise_event(void) {
    return raise_zmk_activity_state_changed(
        (struct zmk_activity_state_changed){.state = activity_state});
}

int set_state(enum zmk_activity_state state) {
    if (activity_state == state)
        return 0;

    activity_state = state;
    return raise_event();
}

enum zmk_activity_state zmk_activity_get_state(void) { return activity_state; }

__weak void receiver_screen_note_input(void) {}
static int note_activity(void) {
    receiver_screen_note_input();
    activity_last_uptime = k_uptime_get();

    return set_state(ZMK_ACTIVITY_ACTIVE);
}

static int activity_event_listener(const zmk_event_t *eh) { return note_activity(); }

void activity_work_handler(struct k_work *work) {
    int64_t inactive_time = k_uptime_get() - activity_last_uptime;
    uint32_t sleep_timeout = zmk_activity_get_sleep_ms();
    uint32_t idle_timeout = zmk_activity_get_idle_ms();

#if IS_ENABLED(CONFIG_ZMK_SLEEP)
    // A timeout of 0 means "never".
    if (sleep_timeout > 0 && inactive_time > (int64_t)sleep_timeout && !is_usb_power_present()) {
        // Put devices in suspend power mode before sleeping
        set_state(ZMK_ACTIVITY_SLEEP);

        if (zmk_pm_suspend_devices() < 0) {
            LOG_ERR("Failed to suspend all the devices");
            zmk_pm_resume_devices();
            return;
        }

        sys_poweroff();
        return;
    }
#endif /* IS_ENABLED(CONFIG_ZMK_SLEEP) */

    if (idle_timeout > 0 && inactive_time > (int64_t)idle_timeout) {
        set_state(ZMK_ACTIVITY_IDLE);
    }
}

K_WORK_DEFINE(activity_work, activity_work_handler);

void activity_expiry_function(struct k_timer *_timer) { k_work_submit(&activity_work); }

K_TIMER_DEFINE(activity_timer, activity_expiry_function, NULL);

static int activity_init(void) {
    activity_last_uptime = k_uptime_get();

    k_timer_start(&activity_timer, K_SECONDS(1), K_SECONDS(1));
    return 0;
}

ZMK_LISTENER(activity, activity_event_listener);
ZMK_SUBSCRIPTION(activity, zmk_position_state_changed);
ZMK_SUBSCRIPTION(activity, zmk_sensor_event);

#if IS_ENABLED(CONFIG_ZMK_POINTING)

static void note_activity_work_cb(struct k_work *_work) { note_activity(); }

K_WORK_DEFINE(note_activity_work, note_activity_work_cb);

static void activity_input_listener(struct input_event *ev, void *user_data) {
    k_work_submit(&note_activity_work);
}

INPUT_CALLBACK_DEFINE(NULL, activity_input_listener, NULL);

#endif

/*
 * Let a driver that acts on its own (a pointing device synthesizing keycodes,
 * for instance) keep the keyboard awake.  Submitting the work instead of
 * calling note_activity() directly keeps the activity state change - and the
 * backlight update it triggers - on the system work queue.
 */
void zmk_activity_note(void) {
#if IS_ENABLED(CONFIG_ZMK_POINTING)
    k_work_submit(&note_activity_work);
#else
    note_activity();
#endif
}

SYS_INIT(activity_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
