/*
 * layer_indicator.c - 根据当前 Layer 控制 LED
 *
 * Layer 0: LED 关闭
 * Layer 1: LED 常亮
 * Layer 2: LED 循环动画（渐亮渐暗）
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>
#include <zmk/keymap.h>

LOG_MODULE_REGISTER(layer_indicator, CONFIG_ZMK_LOG_LEVEL);

/* ==== Devicetree chosen 节点 ==== */
#if !DT_HAS_CHOSEN(zmk_layer_indicator)
#error "Missing chosen node: zmk,layer_indicator"
#endif

static const struct device *led_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_layer_indicator));

/* ==== 配置参数 ==== */
#define CYCLE_BRT_STEP 5
#define CYCLE_INTERVAL_MS 20
#define CYCLE_BRT_MAX 100
#define CYCLE_BRT_MIN 10

/* ==== 内部变量 ==== */
static struct k_work_delayable cycle_work;
static int cycle_brightness = 0;
static bool cycle_direction_up = true;

/* ==== 设置 LED 亮度 ==== */
static void set_led_brightness(int brt) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED device not ready");
        return;
    }
    if (brt < 0)
        brt = 0;
    if (brt > CYCLE_BRT_MAX)
        brt = CYCLE_BRT_MAX;

    led_set_brightness(led_dev, 0, brt);
    cycle_brightness = brt;
}

/* ==== 循环动画处理函数 ==== */
static void cycle_work_handler(struct k_work *work) {
    int current_layer = zmk_keymap_highest_layer_active();

    if (current_layer == 0) {
        /* Layer 0: LED 关闭 */
        set_led_brightness(0);
    } else if (current_layer == 1) {
        /* Layer 1: LED 常亮 */
        set_led_brightness(CYCLE_BRT_MAX);
    } else if (current_layer == 2) {
        /* Layer 2: 循环渐亮渐暗动画 */
        set_led_brightness(cycle_brightness);

        if (cycle_direction_up) {
            cycle_brightness += CYCLE_BRT_STEP;
            if (cycle_brightness >= CYCLE_BRT_MAX) {
                cycle_brightness = CYCLE_BRT_MAX;
                cycle_direction_up = false;
            }
        } else {
            cycle_brightness -= CYCLE_BRT_STEP;
            if (cycle_brightness <= CYCLE_BRT_MIN) {
                cycle_brightness = CYCLE_BRT_MIN;
                cycle_direction_up = true;
            }
        }
    }

    /* 调度下一次动画 */
    k_work_reschedule(&cycle_work, K_MSEC(CYCLE_INTERVAL_MS));
}

/* ==== 初始化函数 ==== */
static int layer_indicator_init(void) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED device not ready");
        return -ENODEV;
    }

    cycle_brightness = 0;
    cycle_direction_up = true;
    set_led_brightness(0);

    k_work_init_delayable(&cycle_work, cycle_work_handler);
    k_work_schedule(&cycle_work, K_MSEC(CYCLE_INTERVAL_MS));

    LOG_INF("Layer indicator initialized");
    return 0;
}

/* ==== SYS_INIT 调用 ==== */
SYS_INIT(layer_indicator_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
