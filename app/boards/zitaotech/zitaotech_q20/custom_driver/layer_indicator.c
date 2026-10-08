/*
 * Layer indicator (GP0): the LED that shows a different effect per active
 * layer.  Same role as indicator_layer.c / LAYER_INDICATOR_LED_PIN in the
 * stock QMK firmware for this board.
 *
 * Copyright (c) 2025 ZitaoTech
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zmk/backlight.h>
#include <zmk/event_manager.h>
#include <zmk/activity.h>
#include <zmk/keymap.h>
#include <zmk/events/position_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_layer_indicator),
             "layer_indicator: No zmk,layer_indicator chosen node found");

static const struct device *const indiled_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_layer_indicator));

#define CHILD_COUNT(...) +1
#define DT_NUM_CHILD(node_id) (DT_FOREACH_CHILD(node_id, CHILD_COUNT))
#define INDICATOR_LED_NUM_LEDS (DT_NUM_CHILD(DT_CHOSEN(zmk_layer_indicator)))

#define BRT_BLINK_HIGH 100
#define BRT_BLINK_LOW 10
#define BLINK_INTERVAL_MS 500

#define CYCLE_BRT_MIN 10
#define CYCLE_BRT_MAX 100
#define CYCLE_BRT_STEP 5
#define CYCLE_INTERVAL_MS 20
#define POLLING_INTERVAL_MS 100

/*
 * Per-layer effects. ZitaoTech Studio can change them at runtime
 * (zmk-feature-zitaotech-backlight); the numbers match that module's enum, and
 * without the module the boards keep their fixed behaviour.
 */
#define LAYER_EFFECT_BLINK 0
#define LAYER_EFFECT_BREATH 1
#define LAYER_EFFECT_FAST_BLINK 2
#define LAYER_EFFECT_OFF 3
#define LAYER_EFFECT_BACKLIGHT 4

#if IS_ENABLED(CONFIG_ZITAOTECH_BACKLIGHT_CONTROL)
#include <zitaotech/zmk/backlight.h>

static uint8_t layer_effect(int layer) {
    return zitaotech_backlight_layer_effect((uint32_t)layer);
}
#else
static uint8_t layer_effect(int layer) {
    switch (layer) {
    case 1:
        return LAYER_EFFECT_BLINK;
    case 2:
        return LAYER_EFFECT_BREATH;
    case 3:
        return LAYER_EFFECT_FAST_BLINK;
    default:
        return LAYER_EFFECT_BACKLIGHT;
    }
}
#endif

/* Q20 has no shoulder keys, so every position is two lower than on the Q10:
 * the bottom-row &to keys are at 38 and 39, and layer 2 puts &bl BL_DEC,
 * &bl BL_TOG and &bl BL_INC on C, B and M (positions 28, 30 and 32). */
#define LAYER_SWITCH_POSITION_1 38
#define LAYER_SWITCH_POSITION_2 39
#define BACKLIGHT_DEC_POSITION 28
#define BACKLIGHT_TOGGLE_POSITION 30
#define BACKLIGHT_INC_POSITION 32

static bool prev_active = false;
static int prev_layer = -1;
static uint8_t prev_effect = 0xFF;
static bool prev_backlight_on = false;
static uint8_t prev_backlight_brt = 0;
static bool blink_on = false;
static uint8_t cycle_brightness = CYCLE_BRT_MIN;
static bool cycle_direction_up = true;

static struct k_work_delayable polling_work;
static struct k_work_delayable blink_work;
static struct k_work_delayable cycle_work;
static atomic_t layer_indicator_ready;

static void set_led_brightness(uint8_t level) {
    if (!device_is_ready(indiled_dev)) {
        LOG_ERR("Indicator LED device not ready");
        return;
    }
    for (int i = 0; i < INDICATOR_LED_NUM_LEDS; i++) {
        int err = led_set_brightness(indiled_dev, i, level);
        if (err < 0) {
            LOG_ERR("Failed to set LED[%d] brightness: %d", i, err);
        }
    }
}

static void blink_work_handler(struct k_work *work) {
    uint8_t effect = layer_effect(prev_layer);

    if (!prev_active ||
        (effect != LAYER_EFFECT_BLINK && effect != LAYER_EFFECT_FAST_BLINK)) {
        set_led_brightness(0);
        return;
    }

    blink_on = !blink_on;
    set_led_brightness(blink_on ? BRT_BLINK_HIGH : BRT_BLINK_LOW);

    uint32_t interval =
        (effect == LAYER_EFFECT_FAST_BLINK) ? (BLINK_INTERVAL_MS / 2) : BLINK_INTERVAL_MS;
    k_work_reschedule(&blink_work, K_MSEC(interval));
}

static void cycle_work_handler(struct k_work *work) {
    if (!prev_active || layer_effect(prev_layer) != LAYER_EFFECT_BREATH) {
        set_led_brightness(0);
        return;
    }

    set_led_brightness(cycle_brightness);

    if (cycle_direction_up) {
        cycle_brightness += CYCLE_BRT_STEP;
        if (cycle_brightness >= CYCLE_BRT_MAX) {
            cycle_brightness = CYCLE_BRT_MAX;
            cycle_direction_up = false;
        }
    } else {
        if (cycle_brightness < CYCLE_BRT_STEP) {
            cycle_brightness = CYCLE_BRT_MIN;
            cycle_direction_up = true;
        } else {
            cycle_brightness -= CYCLE_BRT_STEP;
        }
    }
    k_work_reschedule(&cycle_work, K_MSEC(CYCLE_INTERVAL_MS));
}

static void polling_work_handler(struct k_work *work) {
    k_work_reschedule(&polling_work, K_MSEC(POLLING_INTERVAL_MS));

    bool active = (zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    int current_layer = zmk_keymap_highest_layer_active();
    bool backlight_on = zmk_backlight_is_on();
    uint8_t backlight_brt = zmk_backlight_get_brt();

    /*
     * The automatic mouse layer (layer 4) is a layer like any other here: it has
     * its own effect setting, which defaults to the backlight brightness.
     */
    uint8_t effect = layer_effect(current_layer);

    if (current_layer != prev_layer || active != prev_active || backlight_on != prev_backlight_on ||
        backlight_brt != prev_backlight_brt || effect != prev_effect) {
        prev_layer = current_layer;
        prev_active = active;
        prev_effect = effect;
        prev_backlight_on = backlight_on;
        prev_backlight_brt = backlight_brt;

        k_work_cancel_delayable(&blink_work);
        k_work_cancel_delayable(&cycle_work);
        blink_on = false;
        cycle_brightness = CYCLE_BRT_MIN;
        cycle_direction_up = true;

        /* Do not keep blink/breath timers running while the keyboard is idle. */
        if (!active) {
            set_led_brightness(0);
            return;
        }

        switch (effect) {
        case LAYER_EFFECT_BACKLIGHT:
            /* Layer 0 - and every layer without its own setting - follows the
             * normal ZMK backlight brightness. */
            set_led_brightness(backlight_on ? backlight_brt : 0);
            break;

        case LAYER_EFFECT_BLINK:
            /* Start bright when normal backlight is off, dim when it is on. */
            blink_on = !backlight_on;
            set_led_brightness(blink_on ? BRT_BLINK_HIGH : BRT_BLINK_LOW);
            k_work_reschedule(&blink_work, K_MSEC(BLINK_INTERVAL_MS));
            break;

        case LAYER_EFFECT_BREATH:
            /* Breathes independently of the normal backlight. */
            set_led_brightness(cycle_brightness);
            k_work_reschedule(&cycle_work, K_MSEC(CYCLE_INTERVAL_MS));
            break;

        case LAYER_EFFECT_FAST_BLINK:
            /* Blinks at twice the normal blink frequency. */
            blink_on = true;
            set_led_brightness(BRT_BLINK_HIGH);
            k_work_reschedule(&blink_work, K_MSEC(BLINK_INTERVAL_MS / 2));
            break;

        case LAYER_EFFECT_OFF:
        default:
            set_led_brightness(0);
            break;
        }
    }
}

static int layer_indicator_event_listener(const zmk_event_t *eh) {
    if (!atomic_get(&layer_indicator_ready)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state &&
        (ev->position == LAYER_SWITCH_POSITION_1 || ev->position == LAYER_SWITCH_POSITION_2 ||
         ev->position == BACKLIGHT_DEC_POSITION || ev->position == BACKLIGHT_TOGGLE_POSITION ||
         ev->position == BACKLIGHT_INC_POSITION)) {
        /* Read the actual layer/backlight state after the &to or &bl behavior runs. */
        k_work_reschedule(&polling_work, K_MSEC(1));
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(layer_indicator_listener, layer_indicator_event_listener);
ZMK_SUBSCRIPTION(layer_indicator_listener, zmk_position_state_changed);

static int layer_indicator_init(void) {
    if (!device_is_ready(indiled_dev)) {
        LOG_ERR("LED indicator device not ready");
        return -ENODEV;
    }

    prev_active = (zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    prev_layer = -1;
    prev_backlight_on = false;
    prev_backlight_brt = UINT8_MAX;
    k_work_init_delayable(&polling_work, polling_work_handler);
    k_work_init_delayable(&blink_work, blink_work_handler);
    k_work_init_delayable(&cycle_work, cycle_work_handler);

    atomic_set(&layer_indicator_ready, 1);
    k_work_reschedule(&polling_work, K_MSEC(POLLING_INTERVAL_MS));
    return 0;
}

SYS_INIT(layer_indicator_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
