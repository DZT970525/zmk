#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>

#include <zmk/hid_indicators.h>
#include <zmk/backlight.h>
#include "a320_input_handler_peripheral.h"
#include <zmk/activity.h>
#include <zmk/indicator_tp.h>

#define HID_INDICATORS_CAPS_LOCK (1 << 1)

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_indicator_tp),
             "CONFIG_ZMK_INDICATOR_TP enabled but no zmk,indicator_tp chosen node found");

static const struct device *const led_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_indicator_tp));

#define CHILD_COUNT(...) +1
#define DT_NUM_CHILD(node_id) (DT_FOREACH_CHILD(node_id, CHILD_COUNT))
#define INDICATOR_LED_NUM_LEDS (DT_NUM_CHILD(DT_CHOSEN(zmk_indicator_tp)))

#define BRT_MIN 10
#define BRT_MAX 100
#define BRT_LOW 20
#define BRT_STEP 5

#define ANIMATION_INTERVAL_MS 20
#define POLLING_INTERVAL_MS 20
#define AUTO_OFF_DELAY_MS 5000

static struct k_work_delayable polling_work;
static struct k_work_delayable animation_work;
static struct k_work_delayable auto_off_work;

static bool capslock_on = false;
static bool touch_active = false;
static bool animation_increasing = true;
static uint8_t brightness = BRT_MIN;

static uint8_t last_valid_brt = BRT_MAX;
static uint8_t last_backlight_brt = 0;
static bool manual_override = false;
static bool keyboard_active = false;

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

static void auto_off_work_handler(struct k_work *work) {
    if (!capslock_on && !touch_active) {
        manual_override = false;
        set_led_brightness(0);
        LOG_DBG("Auto-off triggered after inactivity");
    }
}

static void animation_work_handler(struct k_work *work) {
    if (!capslock_on) {
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

static void polling_work_handler(struct k_work *work) {
    bool current_capslock = (zmk_hid_indicators_get_current_profile() & HID_INDICATORS_CAPS_LOCK);
    bool current_touch = tp_is_touched();
    bool current_active = (zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    uint8_t current_brt = zmk_backlight_get_brt();

    if (current_active != keyboard_active) {
        keyboard_active = current_active;
        LOG_DBG("Keyboard activity state changed: active=%d", keyboard_active);
        if (keyboard_active) {
            last_backlight_brt = current_brt;
        }
    }

    // --- Caps Lock state ---
    if (current_capslock != capslock_on) {
        capslock_on = current_capslock;

        if (capslock_on) {
            brightness = BRT_MIN;
            animation_increasing = true;
            k_work_reschedule(&animation_work, K_NO_WAIT);
            LOG_DBG("Caps Lock ON - start animation");
        } else {
            k_work_cancel_delayable(&animation_work);
            manual_override = false;
            LOG_DBG("Caps Lock OFF - stop animation");

            if (current_touch) {
                touch_active = true;
                manual_override = true;
                if (keyboard_active) {
                    last_valid_brt = MAX(BRT_MIN, current_brt);
                }
                set_led_brightness(last_valid_brt);
                k_work_cancel_delayable(&auto_off_work);
            } else {
                set_led_brightness(0);
            }
        }
    }

    // --- Touch event ---
    if (!capslock_on && current_touch != touch_active) {
        touch_active = current_touch;

        if (touch_active) {
            manual_override = true;
            if (keyboard_active) {
                last_valid_brt = MAX(BRT_MIN, current_brt);
            }
            set_led_brightness(last_valid_brt);
            k_work_cancel_delayable(&auto_off_work);
        } else {
            k_work_reschedule(&auto_off_work, K_MSEC(AUTO_OFF_DELAY_MS));
        }
    }

    // --- Backlight brightness changed ---
    if (!capslock_on && !touch_active && current_brt != last_backlight_brt && keyboard_active) {
        last_backlight_brt = current_brt;

        if (current_brt > 0) {
            manual_override = true;
            last_valid_brt = MAX(BRT_MIN, current_brt);
            set_led_brightness(last_valid_brt);
            k_work_reschedule(&auto_off_work, K_MSEC(AUTO_OFF_DELAY_MS));
        }
    }

    k_work_reschedule(&polling_work, K_MSEC(POLLING_INTERVAL_MS));
}

uint8_t indicator_tp_get_last_valid_brightness(void) { return last_valid_brt; }

static int indicator_tp_init(void) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED indicator_tp device not ready");
        return -ENODEV;
    }

    set_led_brightness(0); // 开机默认熄灭
    last_backlight_brt = zmk_backlight_get_brt();
    manual_override = false;
    capslock_on = false;
    touch_active = false;
    keyboard_active = false;

    k_work_init_delayable(&polling_work, polling_work_handler);
    k_work_init_delayable(&animation_work, animation_work_handler);
    k_work_init_delayable(&auto_off_work, auto_off_work_handler);

    k_work_reschedule(&polling_work, K_NO_WAIT);

    return 0;
}

SYS_INIT(indicator_tp_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
