/*
 * Copyright (c) 2023 ZitaoTech
 * SPDX-License-Identifier: MIT
 *
 * Trackpad LED (PWM3 on P0.10) for the ZitaoTech Q20 (nRF52840).
 *
 * Three indications:
 *   - a touch lights it up for touch_on_ms(), the same duration the automatic
 *     mouse layer is held, at a brightness derived from the current DPI;
 *   - Caps Lock turns it into a breathing animation (see apply_capslock_state);
 *   - on USB the transport is announced with a 100 ms flash every second, so
 *     USB and BLE can be told apart on this dual-transport board.  The first
 *     touch ends the announcement until USB mode is entered again.
 * Direction mode additionally blanks the LED for ARROW_BLANK_MS on every arrow
 * key (see indicator_tp_arrow_fired), so the pad's mode is visible at a glance.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/hid_indicators.h>

#include "trackpad_led.h"
#include "a320.h"
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
#include <zitaotech/zmk/trackpad.h>
#endif

#define HID_INDICATORS_CAPS_LOCK (1 << 1)

/*
 * Caps Lock comes from the endpoint profile the ZitaoTech trackpad module
 * fixes the lock state on, rather than from "the current profile": the
 * selected endpoint drops to the empty NONE slot while the USB bus is being
 * reset, and that used to switch the Caps Lock indication off behind the
 * user's back.
 */
static bool caps_lock_active(void) {
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    return (zitaotech_caps_indicators() & HID_INDICATORS_CAPS_LOCK) != 0;
#else
    return (zmk_hid_indicators_get_current_profile() & HID_INDICATORS_CAPS_LOCK) != 0;
#endif
}

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
#define ANIMATION_INTERVAL_MS 20
#define BOOT_BRIGHTNESS 40
#define BOOT_ON_MS 3000
#define TOUCH_ON_MS 1000
#define USB_FLASH_ON_MS 100
#define USB_FLASH_PERIOD_MS 1000
/* How long an arrow key blanks the trackpad LED in direction mode. */
#define ARROW_BLANK_MS 70

static struct k_work motion_work;
static struct k_work dpi_changed_work;
static struct k_work capslock_work;
static struct k_work transport_work;
static struct k_work arrow_blank_work;
static struct k_work_delayable touch_off_work;
static struct k_work_delayable boot_off_work;
static struct k_work_delayable animation_work;
static struct k_work_delayable usb_flash_work;
static struct k_work_delayable arrow_blank_restore_work;

/*
 * The pad has to light up the instant a finger lands on it.  Submitting the
 * brightness update to the system work queue means waiting behind whatever
 * else is queued there (Studio RPC, settings writes, the other Studio
 * modules), which shows up as a visible delay on touch.  The reaction
 * therefore gets its own cooperative queue, which outranks the system work
 * queue and runs as soon as the MOTION interrupt has returned.
 */
#define INDICATOR_WORKQ_STACK_SIZE 1024
#define INDICATOR_WORKQ_PRIORITY K_PRIO_COOP(2)

K_THREAD_STACK_DEFINE(indicator_workq_stack, INDICATOR_WORKQ_STACK_SIZE);
static struct k_work_q indicator_workq;
static atomic_t indicator_ready;
static atomic_t requested_capslock;
static atomic_t requested_usb_mode;

static bool capslock_on;
static bool boot_active;
static bool touch_active;
static bool animation_increasing = true;
static bool usb_mode;
static bool usb_flash_active;
static bool usb_flash_state;
/* An arrow key is blanking the LED right now (direction-mode feedback). */
static bool arrow_blanking;
static uint8_t brightness = BRT_MIN;
static uint8_t last_valid_brt = BRT_MAX;

static void set_led_brightness(uint8_t level);
uint8_t indicator_tp_get_last_valid_brightness(void);

/*
 * How long the LED (and, in the module, the automatic mouse layer) stays on
 * after the last movement.  Studio can change it, so read it back every time
 * instead of using the compile-time default.
 */
static uint32_t touch_on_ms(void) {
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    return zitaotech_trackpad_touch_on_ms();
#else
    return TOUCH_ON_MS;
#endif
}

static void restore_non_usb_led_state(void) {
    if (capslock_on) {
        brightness = BRT_MIN;
        animation_increasing = true;
        k_work_reschedule(&animation_work, K_NO_WAIT);
    } else if (touch_active) {
        set_led_brightness(indicator_tp_get_last_valid_brightness());
    } else if (boot_active) {
        set_led_brightness(BOOT_BRIGHTNESS);
    } else {
        set_led_brightness(0);
    }
}

static void usb_flash_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!usb_flash_active) {
        return;
    }

    usb_flash_state = !usb_flash_state;
    set_led_brightness(usb_flash_state ? BRT_MAX : 0);
    k_work_reschedule(
        &usb_flash_work,
        K_MSEC(usb_flash_state ? USB_FLASH_ON_MS : USB_FLASH_PERIOD_MS - USB_FLASH_ON_MS));
}

static void transport_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    bool new_usb_mode = atomic_get(&requested_usb_mode) != 0;
    if (new_usb_mode == usb_mode) {
        return;
    }

    usb_mode = new_usb_mode;
    if (usb_mode) {
        k_work_cancel_delayable(&animation_work);
        usb_flash_active = true;
        usb_flash_state = false;
        k_work_reschedule(&usb_flash_work, K_NO_WAIT);
    } else {
        k_work_cancel_delayable(&usb_flash_work);
        usb_flash_active = false;
        usb_flash_state = false;
        restore_non_usb_led_state();
    }
}

static void apply_capslock_state(bool enabled) {
    if (enabled == capslock_on) {
        return;
    }

    capslock_on = enabled;
    if (usb_flash_active) {
        return;
    }
    if (capslock_on) {
        brightness = BRT_MIN;
        animation_increasing = true;
        k_work_reschedule(&animation_work, K_NO_WAIT);
    } else {
        k_work_cancel_delayable(&animation_work);
        if (touch_active) {
            set_led_brightness(indicator_tp_get_last_valid_brightness());
        } else if (boot_active) {
            set_led_brightness(BOOT_BRIGHTNESS);
        } else {
            set_led_brightness(0);
        }
    }
}

static void capslock_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    apply_capslock_state(atomic_get(&requested_capslock) != 0);
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

uint8_t indicator_tp_get_last_valid_brightness(void) {
    last_valid_brt = MAX(BRT_MIN, a320_dpi_get());
    return last_valid_brt;
}

static void boot_off_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    boot_active = false;
    if (!usb_flash_active && !capslock_on && !touch_active) {
        set_led_brightness(0);
    }
}

static void touch_off_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    touch_active = false;
    if (usb_flash_active) {
        return;
    }
    if (!capslock_on && !boot_active) {
        set_led_brightness(0);
    } else if (boot_active && !capslock_on) {
        set_led_brightness(BOOT_BRIGHTNESS);
    }
}

/* Runs in the system work queue, not in the GPIO ISR. */
static void motion_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    bool stopped_usb_flash = false;

    /* The first touch ends the USB indication until USB mode is entered again. */
    if (usb_flash_active) {
        usb_flash_active = false;
        usb_flash_state = false;
        k_work_cancel_delayable(&usb_flash_work);
        stopped_usb_flash = true;
    }

    touch_active = true;
    if (stopped_usb_flash) {
        restore_non_usb_led_state();
    } else if (!capslock_on && !arrow_blanking) {
        set_led_brightness(indicator_tp_get_last_valid_brightness());
    }
    k_work_reschedule(&touch_off_work, K_MSEC(touch_on_ms()));
}

void indicator_tp_motion_triggered(void) {
    if (atomic_get(&indicator_ready)) {
        k_work_submit_to_queue(&indicator_workq, &motion_work);
    }
}

/*
 * Direction-mode feedback.
 *
 * Pointer mode keeps the LED lit for as long as a finger is moving; direction
 * mode blanks it for ARROW_BLANK_MS every time it fires an arrow key, so which
 * mode the trackpad is in is obvious at a glance.
 */
static void arrow_blank_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    /* The USB transport announcement owns the LED while it flashes. */
    if (usb_flash_active) {
        return;
    }

    arrow_blanking = true;
    set_led_brightness(0);
    k_work_reschedule_for_queue(&indicator_workq, &arrow_blank_restore_work,
                                K_MSEC(ARROW_BLANK_MS));
}

static void arrow_blank_restore_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    arrow_blanking = false;
    if (usb_flash_active) {
        return;
    }
    restore_non_usb_led_state();
}

void indicator_tp_arrow_fired(void) {
    if (atomic_get(&indicator_ready)) {
        k_work_submit_to_queue(&indicator_workq, &arrow_blank_work);
    }
}

static void dpi_changed_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (usb_flash_active || capslock_on) {
        return;
    }

    set_led_brightness(indicator_tp_get_last_valid_brightness());
    if (!touch_active) {
        k_work_reschedule(&touch_off_work, K_MSEC(touch_on_ms()));
    }
}

void indicator_tp_dpi_changed(void) {
    if (atomic_get(&indicator_ready)) {
        k_work_submit_to_queue(&indicator_workq, &dpi_changed_work);
    }
}

/*
 * DPI can also change from ZitaoTech Studio (the Trackpad tab), so listen to
 * the trackpad module's event instead of only reacting to the &DPI keys.
 */
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
static int tp_dpi_changed_listener(const zmk_event_t *eh) {
    if (!as_zitaotech_trackpad_dpi_changed(eh)) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    indicator_tp_dpi_changed();
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(tp_dpi_changed, tp_dpi_changed_listener);
ZMK_SUBSCRIPTION(tp_dpi_changed, zitaotech_trackpad_dpi_changed);
#endif

static void animation_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (usb_flash_active || !capslock_on) {
        return;
    }

    if (animation_increasing) {
        brightness += BRT_STEP;
        if (brightness >= BRT_MAX) {
            brightness = BRT_MAX;
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

static int trackpad_led_hid_listener(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Read the profile the host reports LEDs into instead of the event's value:
     * the event carries the *selected* endpoint's profile, which is the empty
     * NONE slot while the USB bus is being reset. */
    bool new_capslock = caps_lock_active();
    atomic_set(&requested_capslock, new_capslock);
    if (atomic_get(&indicator_ready)) {
        /* Serialize LED state changes with MOTION and timeout work. */
        k_work_submit(&capslock_work);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(trackpad_led_listener, trackpad_led_hid_listener);
ZMK_SUBSCRIPTION(trackpad_led_listener, zmk_hid_indicators_changed);

/* The transport indication only makes sense on a board with a radio: USB
 * flashes, BLE is dark. */
static int trackpad_led_endpoint_listener(const zmk_event_t *eh) {
    const struct zmk_endpoint_changed *ev = as_zmk_endpoint_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    atomic_set(&requested_usb_mode, ev->endpoint.transport == ZMK_TRANSPORT_USB);
    if (atomic_get(&indicator_ready)) {
        k_work_submit(&transport_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(trackpad_led_endpoint, trackpad_led_endpoint_listener);
ZMK_SUBSCRIPTION(trackpad_led_endpoint, zmk_endpoint_changed);

static int indicator_tp_init(void) {
    if (!device_is_ready(led_dev)) {
        return -ENODEV;
    }

    boot_active = true;
    indicator_tp_get_last_valid_brightness();
    atomic_set(&requested_capslock, caps_lock_active());
    atomic_set(&requested_usb_mode,
               zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB);

    k_work_init(&motion_work, motion_work_handler);
    k_work_init(&dpi_changed_work, dpi_changed_work_handler);
    k_work_init(&capslock_work, capslock_work_handler);
    k_work_init(&transport_work, transport_work_handler);
    k_work_init(&arrow_blank_work, arrow_blank_work_handler);
    k_work_init_delayable(&touch_off_work, touch_off_work_handler);
    k_work_init_delayable(&boot_off_work, boot_off_work_handler);
    k_work_init_delayable(&animation_work, animation_work_handler);
    k_work_init_delayable(&usb_flash_work, usb_flash_work_handler);
    k_work_init_delayable(&arrow_blank_restore_work, arrow_blank_restore_work_handler);
    k_work_queue_start(&indicator_workq, indicator_workq_stack,
                       K_THREAD_STACK_SIZEOF(indicator_workq_stack), INDICATOR_WORKQ_PRIORITY,
                       NULL);
    set_led_brightness(BOOT_BRIGHTNESS);
    atomic_set(&indicator_ready, 1);
    k_work_submit(&capslock_work);
    k_work_submit(&transport_work);
    k_work_reschedule(&boot_off_work, K_MSEC(BOOT_ON_MS));
    return 0;
}

SYS_INIT(indicator_tp_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
