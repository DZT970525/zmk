/*
 * keyboard_backlight.c - 控制键盘背光（开机渐入 + idle 渐暗 + 唤醒渐亮）
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>
#include <zmk/activity.h>
#include <zmk/backlight.h>

LOG_MODULE_REGISTER(keyboard_backlight, CONFIG_ZMK_LOG_LEVEL);

/* ==== Devicetree LED 节点 ==== */
#define KEYBOARD_BACKLIGHT_NODE DT_NODELABEL(keyboard_backlight)
#if !DT_NODE_HAS_STATUS(KEYBOARD_BACKLIGHT_NODE, okay)
#error "Missing DT node: keyboard_backlight"
#endif

static const struct device *led_dev = DEVICE_DT_GET(KEYBOARD_BACKLIGHT_NODE);

/* ==== 配置参数 ==== */
#ifndef CONFIG_KEYBOARD_BACKLIGHT_BRT
#define CONFIG_KEYBOARD_BACKLIGHT_BRT 100
#endif

#define CYCLE_BRT_STEP 5
#define CYCLE_INTERVAL_MS 50

/* ==== 内部变量 ==== */
static int current_brt = 0; /* 初始为 0，开机渐入 */
static bool keyboard_active = false;
static struct k_work_delayable cycle_work;
static bool fade_direction_up = true; /* true 表示亮度上升，false 表示下降 */

/* ==== 设置背光亮度 ==== */
static void set_backlight(int brt) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED device not ready");
        return;
    }
    if (brt < 0)
        brt = 0;
    if (brt > CONFIG_KEYBOARD_BACKLIGHT_BRT)
        brt = CONFIG_KEYBOARD_BACKLIGHT_BRT;

    led_set_brightness(led_dev, 0, brt);
    current_brt = brt;
}

/* ==== 循环工作处理函数，实现渐暗/渐亮动画 ==== */
static void cycle_work_handler(struct k_work *work) {
    bool current_active = (zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);

    /* 检测状态变化 */
    if (current_active != keyboard_active) {
        keyboard_active = current_active;
        LOG_DBG("Keyboard activity state changed: active=%d", keyboard_active);

        if (keyboard_active) {
            /* 活动时渐亮 */
            fade_direction_up = true;
        } else {
            /* 空闲时渐暗 */
            fade_direction_up = false;
        }
    }

    /* 根据 fade_direction 执行动画 */
    if (fade_direction_up) {
        current_brt += CYCLE_BRT_STEP;
        if (current_brt >= CONFIG_KEYBOARD_BACKLIGHT_BRT) {
            current_brt = CONFIG_KEYBOARD_BACKLIGHT_BRT;
            fade_direction_up = false; /* 到达最大亮度停止渐入 */
        }
    } else {
        if (!keyboard_active && current_brt > 0) {
            current_brt -= CYCLE_BRT_STEP;
            if (current_brt < 0)
                current_brt = 0;
        }
    }

    set_backlight(current_brt);

    /* 调度下一次动画 */
    k_work_reschedule(&cycle_work, K_MSEC(CYCLE_INTERVAL_MS));
}

/* ==== 初始化函数 ==== */
static int keyboard_backlight_init(void) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED device not ready");
        return -ENODEV;
    }

    /* 开机渐入，从 0 开始 */
    current_brt = 0;
    keyboard_active = true; /* 开机默认认为键盘活动 */
    fade_direction_up = true;

    set_backlight(current_brt);

    k_work_init_delayable(&cycle_work, cycle_work_handler);
    k_work_schedule(&cycle_work, K_MSEC(CYCLE_INTERVAL_MS));

    LOG_INF("Keyboard backlight initialized, starting fade-in animation");
    return 0;
}

/* ==== SYS_INIT 调用 ==== */
SYS_INIT(keyboard_backlight_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
