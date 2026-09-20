/*
 * Copyright (c) 2023 ZitaoTech
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/hid_indicators.h>
#include "a320.h"
#include "trackpad_led.h"

#define HID_INDICATORS_CAPS_LOCK (1 << 1)

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_trackpad_led),
             "CONFIG_ZMK_TRACKPAD_LED enabled but no zmk,trackpad_led chosen node found");

static const struct device *const led_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_trackpad_led));

#define CHILD_COUNT(...) +1
#define DT_NUM_CHILD(node_id) (DT_FOREACH_CHILD(node_id, CHILD_COUNT))
#define INDICATOR_LED_NUM_LEDS (DT_NUM_CHILD(DT_CHOSEN(zmk_trackpad_led)))

#define BRT_MIN 10
#define BRT_MAX 100
#define BRT_LOW 20
#define BRT_STEP 5
#define CAPS_BRT_MAX MAX(BRT_LOW, CONFIG_TRACKPAD_LED_BRIGHTNESS)

#define ANIMATION_INTERVAL_MS 20
#define POLLING_INTERVAL_MS 5
#define AUTO_OFF_DELAY_MS 1000
#define TOUCH_RELEASE_DELAY_MS 50

#define FLASH_ON_MS 100   /* USB mode ON duration */
#define FLASH_PERIOD 1000 /* Total USB flash period */

static struct k_work_delayable polling_work;
static struct k_work_delayable animation_work;
static struct k_work_delayable auto_off_work;
static struct k_work_delayable usb_flash_work;
static struct k_work_delayable touch_release_work;
static struct k_work motion_work;
static struct k_work dpi_changed_work;
static struct k_work capslock_work;

/* The A320 GPIO interrupt is enabled before APPLICATION init runs. */
static atomic_t indicator_ready;
static atomic_t requested_capslock;

static bool capslock_on = false;
static bool touch_active = false;
static bool animation_increasing = true;
static uint8_t brightness = BRT_MIN;

static uint8_t last_valid_brt = BRT_MAX;
static uint8_t last_dpi_brt = BRT_MAX;

static bool usb_flash_state = false; /* true = LED on, false = LED off */
static bool usb_mode = false;        /* Whether currently in USB transport mode */

static void set_led_brightness(uint8_t level) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED device not ready");
        return;
    }
    for (int i = 0; i < INDICATOR_LED_NUM_LEDS; i++) {
        int err = led_set_brightness(led_dev, i, level);
        if (err < 0) {
            LOG_ERR("Failed to set LED[%d] brightness: %d", i, err);
        }
    }
}

/* USB flashing handler */
static void usb_flash_work_handler(struct k_work *work) {
    if (!usb_mode) {
        /* If no longer in USB mode, ensure LED is off and exit */
        set_led_brightness(0);
        return;
    }

    usb_flash_state = !usb_flash_state;
    set_led_brightness(usb_flash_state ? BRT_MAX : 0);

    /* Schedule next toggle: LED stays on for FLASH_ON_MS, off for the rest */
    k_work_reschedule(&usb_flash_work,
                      K_MSEC(usb_flash_state ? FLASH_ON_MS : (FLASH_PERIOD - FLASH_ON_MS)));
}

static void auto_off_work_handler(struct k_work *work) {
    if (!capslock_on && !touch_active) {
        set_led_brightness(0);
        LOG_DBG("Auto-off triggered after inactivity");
    }
}

static void touch_release_work_handler(struct k_work *work) {
    touch_active = false;

    if (!capslock_on) {
        k_work_reschedule(&auto_off_work, K_MSEC(AUTO_OFF_DELAY_MS));
    }
}

/* Runs as soon as the A320 MOTION pin goes active (low). */
static void motion_work_handler(struct k_work *work) {
    if (zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB || capslock_on) {
        return;
    }

    touch_active = true;
    last_valid_brt = MAX(BRT_MIN, a320_dpi_get());
    last_dpi_brt = last_valid_brt;

    set_led_brightness(last_valid_brt);
    k_work_cancel_delayable(&auto_off_work);
    k_work_reschedule(&touch_release_work, K_MSEC(TOUCH_RELEASE_DELAY_MS));
}

void indicator_tp_motion_triggered(void) {
    if (atomic_get(&indicator_ready)) {
        k_work_submit(&motion_work);
    }
}

/* Show a newly selected DPI without pretending that the pad was touched. */
static void dpi_changed_work_handler(struct k_work *work) {
    if (zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB || capslock_on) {
        return;
    }

    last_valid_brt = MAX(BRT_MIN, a320_dpi_get());
    last_dpi_brt = last_valid_brt;
    set_led_brightness(last_valid_brt);

    k_work_cancel_delayable(&auto_off_work);
    if (!touch_active) {
        k_work_reschedule(&auto_off_work, K_MSEC(AUTO_OFF_DELAY_MS));
    }
}

void indicator_tp_dpi_changed(void) {
    if (atomic_get(&indicator_ready)) {
        k_work_submit(&dpi_changed_work);
    }
}

static void apply_capslock_state(bool new_capslock) {
    /* USB flashing has priority; apply the latest CapsLock state after returning to BLE. */
    if (zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB) {
        return;
    }

    if (new_capslock == capslock_on) {
        return;
    }

    capslock_on = new_capslock;

    if (capslock_on) {
        /* Start visibly but avoid an abrupt 100% PWM jump on Caps Lock. */
        brightness = BRT_LOW;
        animation_increasing = true;
        set_led_brightness(brightness);
        k_work_cancel_delayable(&auto_off_work);
        k_work_reschedule(&animation_work, K_MSEC(ANIMATION_INTERVAL_MS));
    } else {
        k_work_cancel_delayable(&animation_work);

        if (touch_active) {
            last_valid_brt = MAX(BRT_MIN, a320_dpi_get());
            last_dpi_brt = last_valid_brt;
            set_led_brightness(last_valid_brt);
            k_work_cancel_delayable(&auto_off_work);
        } else {
            set_led_brightness(0);
        }
    }
}

static void capslock_work_handler(struct k_work *work) {
    apply_capslock_state(atomic_get(&requested_capslock) != 0);
}

static int trackpad_led_hid_indicators_listener(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    bool new_capslock = (ev->indicators & HID_INDICATORS_CAPS_LOCK) != 0;
    atomic_set(&requested_capslock, new_capslock);

    if (atomic_get(&indicator_ready)) {
        /* Do not run the PWM driver in the HID event callback. */
        k_work_submit(&capslock_work);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(trackpad_led_hid_listener, trackpad_led_hid_indicators_listener);
ZMK_SUBSCRIPTION(trackpad_led_hid_listener, zmk_hid_indicators_changed);

static void animation_work_handler(struct k_work *work) {
    if (!capslock_on)
        return;

    if (animation_increasing) {
        brightness += BRT_STEP;
        if (brightness >= CAPS_BRT_MAX) {
            brightness = CAPS_BRT_MAX;
            animation_increasing = false;
        }
    } else {
        brightness -= BRT_STEP;
        if (brightness <= BRT_LOW) {
            brightness = BRT_LOW;
            animation_increasing = true;
        }
    }

    set_led_brightness(brightness);
    k_work_reschedule(&animation_work, K_MSEC(ANIMATION_INTERVAL_MS));
}

static void polling_work_handler(struct k_work *work) {
    enum zmk_transport transport = zmk_endpoints_selected().transport;
    uint8_t current_dpi_brt = MAX(BRT_MIN, a320_dpi_get());

    if (transport == ZMK_TRANSPORT_USB) {
        if (!usb_mode) {
            usb_mode = true;
            capslock_on = false;
            usb_flash_state = false;
            k_work_cancel_delayable(&animation_work);
            k_work_cancel_delayable(&auto_off_work);
            k_work_reschedule(&usb_flash_work, K_NO_WAIT);
            LOG_INF("Entered USB flash mode");
        }
        k_work_reschedule(&polling_work, K_MSEC(POLLING_INTERVAL_MS));
        return;
    }

    if (usb_mode) {
        usb_mode = false;
        k_work_cancel_delayable(&usb_flash_work);
        set_led_brightness(0);
        LOG_INF("Exited USB flash mode");
        k_work_submit(&capslock_work);
        k_work_reschedule(&polling_work, K_MSEC(POLLING_INTERVAL_MS));
        return;
    }

    if (!capslock_on && !touch_active && current_dpi_brt != last_dpi_brt) {
        last_dpi_brt = current_dpi_brt;
        last_valid_brt = current_dpi_brt;
        set_led_brightness(last_valid_brt);
        k_work_reschedule(&auto_off_work, K_MSEC(AUTO_OFF_DELAY_MS));
    }

    k_work_reschedule(&polling_work, K_MSEC(POLLING_INTERVAL_MS));
}

uint8_t indicator_tp_get_last_valid_brightness(void) { return last_valid_brt; }

static int indicator_tp_init(void) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED indicator_tp device not ready");
        return -ENODEV;
    }

    set_led_brightness(0);
    usb_mode = false;
    usb_flash_state = false;
    last_valid_brt = MAX(BRT_MIN, a320_dpi_get());
    last_dpi_brt = last_valid_brt;
    capslock_on = touch_active = false;
    atomic_set(&requested_capslock,
               (zmk_hid_indicators_get_current_profile() & HID_INDICATORS_CAPS_LOCK) != 0);

    k_work_init_delayable(&polling_work, polling_work_handler);
    k_work_init_delayable(&animation_work, animation_work_handler);
    k_work_init_delayable(&auto_off_work, auto_off_work_handler);
    k_work_init_delayable(&usb_flash_work, usb_flash_work_handler);
    k_work_init_delayable(&touch_release_work, touch_release_work_handler);
    k_work_init(&motion_work, motion_work_handler);
    k_work_init(&dpi_changed_work, dpi_changed_work_handler);
    k_work_init(&capslock_work, capslock_work_handler);

    atomic_set(&indicator_ready, 1);
    k_work_submit(&capslock_work);
    k_work_reschedule(&polling_work, K_NO_WAIT);
    return 0;
}

SYS_INIT(indicator_tp_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
