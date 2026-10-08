/*
 * Trackpad LED (left half).
 *
 * The pad lights up while a hand is on it - the A320 reports activity the
 * moment it is touched - and, in direction mode, blinks off for a moment every
 * time it fires an arrow key so the two modes can be told apart at a glance.
 * The TrackPoint drives nothing here: this LED belongs to the pad.
 *
 * Copyright (c) 2023 ZitaoTech
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zmk/event_manager.h>
#include <zmk/endpoints.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/hid_indicators.h>

#include "trackpad_led.h"
#include "a320.h"
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
#include <zitaotech/zmk/trackpad.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_trackpad_led),
             "CONFIG_ZMK_TRACKPAD_LED enabled but no zmk,trackpad_led chosen node found");

static const struct device *const led_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_trackpad_led));

#define CHILD_COUNT(...) +1
#define DT_NUM_CHILD(node_id) (DT_FOREACH_CHILD(node_id, CHILD_COUNT))
#define INDICATOR_LED_NUM_LEDS (DT_NUM_CHILD(DT_CHOSEN(zmk_trackpad_led)))

#define BRT_MIN 10
/* Fallback when the trackpad module is not built in. */
#define TOUCH_ON_MS 1000
/* How long an arrow key blanks the pad in direction mode. */
#define ARROW_BLANK_MS 70

/*
 * The LED updates run on their own cooperative queue: it outranks the system
 * work queue (Studio RPC, settings writes), so the pad lights up as soon as
 * the MOTION interrupt has returned instead of behind whatever is queued.
 */
#define INDICATOR_WORKQ_STACK_SIZE 1024
#define INDICATOR_WORKQ_PRIORITY K_PRIO_COOP(2)

K_THREAD_STACK_DEFINE(indicator_workq_stack, INDICATOR_WORKQ_STACK_SIZE);
static struct k_work_q indicator_workq;
static atomic_t indicator_ready;

static struct k_work motion_work;
static struct k_work arrow_blank_work;
static struct k_work_delayable touch_off_work;
static struct k_work_delayable arrow_blank_restore_work;

static bool usb_flash_on;
/* Latched until USB is physically disconnected. */
static atomic_t usb_flash_cancelled;
static struct k_work_delayable usb_flash_work;
static bool touch_active;
static bool arrow_blanking;
static atomic_t requested_capslock;
static bool capslock_on;
static uint8_t breathing_brightness = 10;
static int breathing_delta = 5;
static struct k_work capslock_work;
static struct k_work_delayable breathing_work;

static bool caps_lock_active(void) {
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    return (zitaotech_caps_indicators() & BIT(1)) != 0;
#else
    return (zmk_hid_indicators_get_current_profile() & BIT(1)) != 0;
#endif
}

static void set_led_brightness(uint8_t level) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("Trackpad LED device not ready");
        return;
    }

    for (int i = 0; i < INDICATOR_LED_NUM_LEDS; i++) {
        int err = led_set_brightness(led_dev, i, level);
        if (err < 0) {
            LOG_ERR("Failed to set trackpad LED[%d] brightness: %d", i, err);
        }
    }
}

uint8_t indicator_tp_get_last_valid_brightness(void) { return MAX(BRT_MIN, a320_dpi_get()); }

/* How long the pad stays "touched" after the last activity (Studio's value). */
static uint32_t touch_on_ms(void) {
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    return zitaotech_trackpad_touch_on_ms();
#else
    return TOUCH_ON_MS;
#endif
}

/* What the LED should show right now: lit while touched, unless an arrow key
 * is blanking it. */
static void apply_led_state(void) {
    if (capslock_on) {
        set_led_brightness(breathing_brightness);
        return;
    }
    if (!touch_active) {
        set_led_brightness(usb_flash_on ? BRT_MIN : 0);
        return;
    }
    set_led_brightness(touch_active && !arrow_blanking
                           ? indicator_tp_get_last_valid_brightness()
                           : 0);
}

/* 100 ms pulse every second, below touch feedback and Caps Lock breathing. */
static bool usb_output_active(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_hid_ready() &&
           zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB;
#else
    return false;
#endif
}

static void usb_flash_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!usb_output_active() || atomic_get(&usb_flash_cancelled)) {
        usb_flash_on = false;
        apply_led_state();
        return;
    }
    usb_flash_on = !usb_flash_on;
    apply_led_state();
    k_work_reschedule_for_queue(&indicator_workq, &usb_flash_work,
                                K_MSEC(usb_flash_on ? 100 : 900));
}

static int usb_led_listener(const zmk_event_t *event) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    const struct zmk_usb_conn_state_changed *usb = as_zmk_usb_conn_state_changed(event);
    if (usb && usb->conn_state == ZMK_USB_CONN_NONE) {
        atomic_set(&usb_flash_cancelled, 0);
    }
#endif
    if (atomic_get(&indicator_ready)) {
        k_work_reschedule_for_queue(&indicator_workq, &usb_flash_work, K_NO_WAIT);
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(trackpad_usb_led, usb_led_listener);
ZMK_SUBSCRIPTION(trackpad_usb_led, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(trackpad_usb_led, zmk_usb_conn_state_changed);
#endif

static void breathing_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!capslock_on) return;
    breathing_brightness += breathing_delta;
    if (breathing_brightness >= 100) { breathing_brightness = 100; breathing_delta = -5; }
    else if (breathing_delta < 0 && breathing_brightness <= 20) { breathing_brightness = 20; breathing_delta = 5; }
    apply_led_state();
    k_work_reschedule_for_queue(&indicator_workq, &breathing_work, K_MSEC(20));
}

static void capslock_handler(struct k_work *work) {
    ARG_UNUSED(work);
    bool enabled = atomic_get(&requested_capslock) != 0;
    if (enabled == capslock_on) return;
    capslock_on = enabled;
    if (enabled) {
        breathing_brightness = 10;
        breathing_delta = 5;
        k_work_reschedule_for_queue(&indicator_workq, &breathing_work, K_NO_WAIT);
    } else {
        k_work_cancel_delayable(&breathing_work);
        apply_led_state();
    }
}

static int capslock_listener(const zmk_event_t *event) {
    ARG_UNUSED(event);
    atomic_set(&requested_capslock, caps_lock_active());
    if (atomic_get(&indicator_ready)) k_work_submit_to_queue(&indicator_workq, &capslock_work);
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(trackpad_capslock_led, capslock_listener);
ZMK_SUBSCRIPTION(trackpad_capslock_led, zmk_hid_indicators_changed);

static void touch_off_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    touch_active = false;
    apply_led_state();
}

/* Runs on the indicator work queue, not in the GPIO ISR. */
static void motion_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    atomic_set(&usb_flash_cancelled, 1);
    usb_flash_on = false;
    k_work_cancel_delayable(&usb_flash_work);
    touch_active = true;
    apply_led_state();
    k_work_reschedule_for_queue(&indicator_workq, &touch_off_work, K_MSEC(touch_on_ms()));
}

void indicator_tp_motion_triggered(void) {
    if (atomic_get(&indicator_ready)) {
        k_work_submit_to_queue(&indicator_workq, &motion_work);
    }
}

/*
 * Direction-mode feedback: pointer mode keeps the pad lit while a hand is on
 * it, direction mode blanks it for ARROW_BLANK_MS on every arrow key.
 */
static void arrow_blank_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    arrow_blanking = true;
    apply_led_state();
    k_work_reschedule_for_queue(&indicator_workq, &arrow_blank_restore_work,
                                K_MSEC(ARROW_BLANK_MS));
}

static void arrow_blank_restore_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    arrow_blanking = false;
    apply_led_state();
}

void indicator_tp_arrow_fired(void) {
    if (atomic_get(&indicator_ready)) {
        k_work_submit_to_queue(&indicator_workq, &arrow_blank_work);
    }
}

/* DPI changes retune the brightness while the pad is in use. */
static void dpi_changed_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    apply_led_state();
}

static struct k_work dpi_changed_work;

void indicator_tp_dpi_changed(void) {
    if (atomic_get(&indicator_ready)) {
        k_work_submit_to_queue(&indicator_workq, &dpi_changed_work);
    }
}

static int indicator_tp_init(void) {
    if (!device_is_ready(led_dev)) {
        return -ENODEV;
    }

    k_work_init_delayable(&usb_flash_work, usb_flash_handler);
    k_work_init(&motion_work, motion_work_handler);
    k_work_init(&capslock_work, capslock_handler);
    k_work_init_delayable(&breathing_work, breathing_handler);
    k_work_init(&arrow_blank_work, arrow_blank_work_handler);
    k_work_init(&dpi_changed_work, dpi_changed_work_handler);
    k_work_init_delayable(&touch_off_work, touch_off_work_handler);
    k_work_init_delayable(&arrow_blank_restore_work, arrow_blank_restore_work_handler);
    k_work_queue_start(&indicator_workq, indicator_workq_stack,
                       K_THREAD_STACK_SIZEOF(indicator_workq_stack), INDICATOR_WORKQ_PRIORITY, NULL);
    atomic_set(&indicator_ready, 1);
    k_work_reschedule_for_queue(&indicator_workq, &usb_flash_work, K_NO_WAIT);
    atomic_set(&requested_capslock, caps_lock_active());
    k_work_submit_to_queue(&indicator_workq, &capslock_work);

    apply_led_state();
    return 0;
}

SYS_INIT(indicator_tp_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
