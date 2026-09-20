/*
 * Copyright (c) 2025
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_trackball_led

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/hid_indicators.h>
#include <zmk/rgb_underglow.h>

#include "bbtrackball_input_handler.h" /* bool trackball_is_moving(void) */

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ==== 配置参数 ==== */
#define BRT_MIN 10
#define BRT_MAX 100
#define BRT_STEP 5
#define ANIMATION_INTERVAL 20 /* ms */
#define AUTO_OFF_DELAY 5000   /* ms */
#define POLL_INTERVAL 50      /* ms */

/* ==== LED 子节点数量 ==== */
#define CHILD_COUNT(...) +1
#define DT_NUM_CHILD(node_id) (DT_FOREACH_CHILD(node_id, CHILD_COUNT))
#define LED_NUM (DT_NUM_CHILD(DT_CHOSEN(zmk_trackball_led)))

/* ==== 设备 ==== */
static const struct device *const led_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_trackball_led));

/* ==== 工作队列 ==== */
static struct k_work_delayable anim_work;
static struct k_work_delayable poll_work;
static struct k_work_delayable off_work;

/* ==== 状态变量 ==== */
static bool caps_on = false;
static bool last_move_state = false;
static bool anim_up = true;
static uint8_t anim_brt = BRT_MIN;

static uint8_t last_ug_brt = 0;

/* ⭐ 新增：最近一次实际点亮的亮度（不含0） */
static uint8_t last_valid_brt = BRT_MIN;

/* ==== 工具函数 ==== */
static void set_led_brightness(uint8_t level) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED device not ready");
        return;
    }
    for (int i = 0; i < LED_NUM; i++) {
        led_set_brightness(led_dev, i, level);
    }

    /* 仅在亮度>0时更新 last_valid_brt */
    if (level > 0) {
        last_valid_brt = level;
    }
}

/* ==== 5 秒自动熄灭 ==== */
static void off_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!caps_on && !trackball_is_moving()) {
        set_led_brightness(0);
        LOG_DBG("Auto-off -> LED off");
    }
}

/* ==== CapsLock 呼吸动画 ==== */
static void anim_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!caps_on)
        return;

    set_led_brightness(anim_brt);

    if (anim_up) {
        anim_brt += BRT_STEP;
        if (anim_brt >= BRT_MAX) {
            anim_brt = BRT_MAX;
            anim_up = false;
        }
    } else {
        anim_brt -= BRT_STEP;
        if (anim_brt <= BRT_MIN) {
            anim_brt = BRT_MIN;
            anim_up = true;
        }
    }
    k_work_reschedule(&anim_work, K_MSEC(ANIMATION_INTERVAL));
}

/* ==== 轮询轨迹球状态 + underglow 亮度变化 ==== */
static void poll_handler(struct k_work *work) {
    ARG_UNUSED(work);
    bool moving = trackball_is_moving();

    struct zmk_led_hsb ug = zmk_rgb_underglow_calc_brt(0);
    uint8_t ug_brt = ug.b;

    if (!caps_on) {
        /* 轨迹球移动亮灯 */
        if (moving) {
            if (!last_move_state) {
                uint8_t brt = ug_brt > 0 ? MAX(BRT_MIN, ug_brt) : BRT_MAX;
                set_led_brightness(brt);
                k_work_cancel_delayable(&off_work);
                LOG_DBG("Trackball moved -> LED on (brt %u)", brt);
            }
        } else {
            if (last_move_state) {
                k_work_reschedule(&off_work, K_MSEC(AUTO_OFF_DELAY));
                LOG_DBG("Trackball stop -> start 5s timer");
            }
        }

        /* underglow 亮度变化亮灯 */
        if (ug_brt != last_ug_brt) {
            last_ug_brt = ug_brt;
            if (ug_brt > 0) {
                uint8_t brt = MAX(BRT_MIN, ug_brt);
                set_led_brightness(brt);
                k_work_reschedule(&off_work, K_MSEC(AUTO_OFF_DELAY));
                LOG_DBG("Underglow brightness changed -> LED on (brt %u)", brt);
            }
        }
    }

    last_move_state = moving;
    k_work_reschedule(&poll_work, K_MSEC(POLL_INTERVAL));
}

/* ==== HID Indicators 监听 ==== */
static int hid_listener(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (!ev)
        return ZMK_EV_EVENT_BUBBLE;

    bool new_caps = ev->indicators & (1 << 1);
    if (new_caps != caps_on) {
        caps_on = new_caps;

        if (caps_on) {
            anim_brt = BRT_MIN;
            anim_up = true;
            k_work_reschedule(&anim_work, K_NO_WAIT);
            LOG_DBG("CapsLock ON -> start animation");
        } else {
            k_work_cancel_delayable(&anim_work);
            set_led_brightness(0);
            LOG_DBG("CapsLock OFF -> stop animation & turn off");

            if (trackball_is_moving()) {
                uint8_t brt = MAX(BRT_MIN, zmk_rgb_underglow_calc_brt(0).b);
                set_led_brightness(brt);
                k_work_cancel_delayable(&off_work);
            } else {
                k_work_reschedule(&off_work, K_MSEC(AUTO_OFF_DELAY));
            }
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

/* ==== 初始化 ==== */
static int trackball_led_init(void) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED device not ready");
        return -ENODEV;
    }

    set_led_brightness(0);
    k_work_init_delayable(&anim_work, anim_handler);
    k_work_init_delayable(&poll_work, poll_handler);
    k_work_init_delayable(&off_work, off_handler);

    last_ug_brt = zmk_rgb_underglow_calc_brt(0).b;
    last_valid_brt = BRT_MIN;

    k_work_schedule(&poll_work, K_NO_WAIT);

    LOG_INF("Trackball LED driver init with %d LEDs", LED_NUM);
    return 0;
}

SYS_INIT(trackball_led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
ZMK_LISTENER(trackball_led_listener, hid_listener);
ZMK_SUBSCRIPTION(trackball_led_listener, zmk_hid_indicators_changed);

/* ==== 对外接口：获取最近一次非0亮度 ==== */
uint8_t trackball_led_get_last_valid_brightness(void) { return last_valid_brt; }
