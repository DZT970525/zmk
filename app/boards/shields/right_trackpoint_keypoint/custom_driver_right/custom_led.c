/*
 * custom_led_backlight_follow.c
 * Show boot brightness and explicit TrackPoint speed adjustments.
 * Ordinary backlight changes (including motion/activity) do not light this LED.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>

#include <zmk/backlight.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include "custom_led.h" // ★ 新增

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_custom_led),
             "Custom LED enabled but no zmk,custom_led chosen node found");

static const struct device *const led_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_custom_led));

#define CHILD_COUNT(...) +1
#define DT_NUM_CHILD(node_id) (DT_FOREACH_CHILD(node_id, CHILD_COUNT))
#define LED_NUM (DT_NUM_CHILD(DT_CHOSEN(zmk_custom_led)))

#define BRT_MIN 10
#define OFF_DELAY_MS 3000
#define SPEED_ADJUST_WINDOW_MS 250
#define SPEED_DOWN_POSITION 35
#define SPEED_UP_POSITION 36
#define LOWER_LEFT_POSITION 49
#define LOWER_RIGHT_POSITION 50

/* Fade configs */
#define FADE_STEP_MS 20
#define FADE_STEPS 20

static struct k_work_delayable auto_off_work;
static struct k_work_delayable poll_work;
static struct k_work_delayable fade_work;

static uint8_t last_brt = 255;
static uint8_t current_brt = 0;
static uint8_t target_brt = 0;
static uint8_t fade_start_brt = 0;
static int fade_step = -1;

/* ★ 对外状态：最近一次有效亮度 */
static uint8_t last_valid_brt = BRT_MIN;
static uint32_t last_speed_adjust_time;
static bool speed_adjust_pending;
static bool lower_left_pressed;
static bool lower_right_pressed;

static int speed_adjust_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->position == LOWER_LEFT_POSITION) {
        lower_left_pressed = ev->state;
    } else if (ev->position == LOWER_RIGHT_POSITION) {
        lower_right_pressed = ev->state;
    }

    bool lower_active = lower_left_pressed || lower_right_pressed;
    if (ev->state && lower_active &&
        (ev->position == SPEED_DOWN_POSITION || ev->position == SPEED_UP_POSITION)) {
        last_speed_adjust_time = k_uptime_get_32();
        speed_adjust_pending = true;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(custom_led_speed_adjust_listener, speed_adjust_listener);
ZMK_SUBSCRIPTION(custom_led_speed_adjust_listener, zmk_position_state_changed);

/* === Immediate apply LED === */
static void apply_led(uint8_t brightness) {
    if (!device_is_ready(led_dev))
        return;

    for (int i = 0; i < LED_NUM; i++) {
        led_set_brightness(led_dev, i, brightness);
    }
    current_brt = brightness;
}

/* === Fade animation handler === */
static void fade_handler(struct k_work *work) {
    if (fade_step < 0)
        return;

    float ratio = (float)fade_step / FADE_STEPS;
    int new_level = fade_start_brt + (int)((target_brt - fade_start_brt) * ratio);

    apply_led(new_level);

    fade_step++;
    if (fade_step > FADE_STEPS) {
        apply_led(target_brt);
        fade_step = -1;
        return;
    }

    k_work_reschedule(&fade_work, K_MSEC(FADE_STEP_MS));
}

/* === Start fade === */
static void fade_to(uint8_t new_brt) {
    fade_start_brt = current_brt;
    target_brt = new_brt;
    fade_step = 0;
    k_work_reschedule(&fade_work, K_MSEC(FADE_STEP_MS));
}

/* === Auto-off timeout → fade-out === */
static void auto_off_handler(struct k_work *work) {
    LOG_INF("Auto-off → fade-out to 0");
    fade_to(0);
}

/* === Poll backlight changes every 10ms === */
static void poll_handler(struct k_work *work) {
    uint8_t brt = zmk_backlight_get_brt();

    if (brt != last_brt) {
        last_brt = brt;

        bool speed_adjusted = speed_adjust_pending && (k_uptime_get_32() - last_speed_adjust_time <=
                                                       SPEED_ADJUST_WINDOW_MS);
        speed_adjust_pending = false;

        if (speed_adjusted) {
            /* A manual decrease to backlight 0 still uses the minimum speed/LED level. */
            uint8_t led_level = MAX(BRT_MIN, brt);
            last_valid_brt = led_level;

            if (current_brt == 0) {
                LOG_INF("Fade-in from dark → %d", led_level);
                fade_to(led_level);
            } else {
                k_work_cancel_delayable(&fade_work);
                fade_step = -1;
                apply_led(led_level);
            }

            k_work_cancel_delayable(&auto_off_work);
            k_work_reschedule(&auto_off_work, K_MSEC(OFF_DELAY_MS));
        }
    }

    k_work_reschedule(&poll_work, K_MSEC(10));
}

/* === Public API === */
uint8_t custom_led_get_last_valid_brightness(void) { return last_valid_brt; }

/* === Init === */
static int init_led_follow(void) {
    if (!device_is_ready(led_dev))
        return -ENODEV;

    k_work_init_delayable(&auto_off_work, auto_off_handler);
    k_work_init_delayable(&poll_work, poll_handler);
    k_work_init_delayable(&fade_work, fade_handler);

    uint8_t boot = zmk_backlight_get_brt();
    uint8_t led_level = (boot == 0) ? 0 : MAX(BRT_MIN, boot);
    last_brt = boot;

    if (led_level > 0) {
        last_valid_brt = led_level; // ★ 初始化
    }

    apply_led(led_level);

    k_work_reschedule(&auto_off_work, K_MSEC(OFF_DELAY_MS));
    k_work_reschedule(&poll_work, K_NO_WAIT);

    LOG_INF("LED backlight follow + fade driver initialized");
    return 0;
}

SYS_INIT(init_led_follow, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
