/*
 * display_backlight.c - Display backlight driver with activity awareness
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

LOG_MODULE_REGISTER(display_backlight, CONFIG_ZMK_LOG_LEVEL);

/* ==== DT device ==== */
#define DISPLAY_BACKLIGHT_NODE DT_CHOSEN(zmk_display_backlight)

#if !DT_NODE_HAS_STATUS(DISPLAY_BACKLIGHT_NODE, okay)
#error "Missing chosen node: zmk,display_backlight"
#endif

static const struct device *backlight_dev = DEVICE_DT_GET(DISPLAY_BACKLIGHT_NODE);

/* ==== Brightness config ==== */
#define ACTIVE_BRIGHTNESS CONFIG_DISPLAY_BACKLIGHT_BRIGHTNESS
#define IDLE_BRIGHTNESS CONFIG_DISPLAY_BACKLIGHT_IDLE_BRIGHTNESS

/* ==== Fade config ==== */
#define FADE_STEP 2
#define FADE_INTERVAL_MS 50

/* ==== State ==== */
static uint8_t current_brightness;
static uint8_t target_brightness;
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER) && !IS_ENABLED(CONFIG_ZMK_SETTINGS_RESET_ON_START)
static bool boot_frame_ready;
#else
static bool boot_frame_ready = true;
#endif

/* ==== Work ==== */
static struct k_work_delayable fade_work;

/* ==== Helpers ==== */
static void set_backlight(uint8_t brightness) {
    if (!device_is_ready(backlight_dev)) {
        return;
    }

    led_set_brightness(backlight_dev, 0, brightness);
}

/* ==== Fade worker ==== */
static void fade_work_handler(struct k_work *work) {
    if (current_brightness == target_brightness) {
        return;
    }

    if (current_brightness < target_brightness) {
        current_brightness = MIN(current_brightness + FADE_STEP, target_brightness);
    } else {
        current_brightness = MAX(current_brightness - FADE_STEP, target_brightness);
    }

    set_backlight(current_brightness);
    k_work_schedule(&fade_work, K_MSEC(FADE_INTERVAL_MS));
}

/* ==== Start fade ==== */
static void fade_to(uint8_t brightness) {
    target_brightness = brightness;
    if (!boot_frame_ready) return;
    k_work_cancel_delayable(&fade_work);
    k_work_schedule(&fade_work, K_NO_WAIT);
}

void receiver_backlight_boot_ready(void) {
    if (boot_frame_ready) return;
    boot_frame_ready = true;
    /* The complete boot frame is ready; reuse the gradual fade from black. */
    fade_to(target_brightness);
}
void receiver_backlight_set_idle(bool idle, unsigned char brightness) {
    fade_to(idle ? brightness : ACTIVE_BRIGHTNESS);
}
void receiver_backlight_preview(unsigned char brightness) {
    k_work_cancel_delayable(&fade_work);
    target_brightness = brightness;
    if (!boot_frame_ready) return;
    current_brightness = brightness;
    set_backlight(brightness);
}
/* ==== Activity listener ==== */
static int activity_listener_cb(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER) && !IS_ENABLED(CONFIG_ZMK_SETTINGS_RESET_ON_START)
    return ZMK_EV_EVENT_BUBBLE;
#endif
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state == ZMK_ACTIVITY_ACTIVE) {
        fade_to(ACTIVE_BRIGHTNESS);
    } else {
        fade_to(IDLE_BRIGHTNESS);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

/* ==== Listener registration ==== */
ZMK_LISTENER(display_backlight_activity_listener, activity_listener_cb);
ZMK_SUBSCRIPTION(display_backlight_activity_listener, zmk_activity_state_changed);

/* ==== Init ==== */
static int display_backlight_init(void) {
    if (!device_is_ready(backlight_dev)) {
        LOG_ERR("Display backlight device not ready");
        return -ENODEV;
    }

    k_work_init_delayable(&fade_work, fade_work_handler);

    current_brightness = 0;
    set_backlight(0);

    fade_to(ACTIVE_BRIGHTNESS);

    LOG_INF("Display backlight initialized (activity controlled)");

    return 0;
}

SYS_INIT(display_backlight_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
