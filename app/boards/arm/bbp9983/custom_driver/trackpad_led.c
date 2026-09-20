/*
 * trackpad_led.c - Control Trackpad LED based on Caps Lock state (polling)
 *
 * Copyright (c) 2025 ZitaoTech
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include <zmk/hid_indicators.h>

LOG_MODULE_REGISTER(trackpad_led, CONFIG_ZMK_LOG_LEVEL);

/* ==== 用户可配置参数 ==== */
#define BRT_MAX CONFIG_TRACKPAD_LED_BRIGHTNESS /* Caps Lock 时 LED 亮度 */

#define POLL_INTERVAL K_MSEC(50) /* 轮询间隔 */

#define HID_INDICATORS_CAPS_LOCK (1 << 1)

#define TRACKPAD_LED_NODE DT_NODELABEL(trackpad_led)

#if !DT_NODE_HAS_STATUS(TRACKPAD_LED_NODE, okay)
#error "Missing DT node: trackpad_led"
#endif

static const struct device *led_dev = DEVICE_DT_GET(TRACKPAD_LED_NODE);
#define TRACKPAD_LED_CHANNEL 0

static struct k_work_delayable trackpad_led_work;

static void trackpad_led_update(struct k_work *work) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED device not ready");
        return;
    }

    bool capslock = (zmk_hid_indicators_get_current_profile() & HID_INDICATORS_CAPS_LOCK);

    if (capslock) {
        led_set_brightness(led_dev, TRACKPAD_LED_CHANNEL, BRT_MAX);
    } else {
        led_off(led_dev, TRACKPAD_LED_CHANNEL);
    }

    /* 继续轮询 */
    k_work_schedule(&trackpad_led_work, POLL_INTERVAL);
}

static int trackpad_led_init(void) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED device not ready");
        return -ENODEV;
    }

    k_work_init_delayable(&trackpad_led_work, trackpad_led_update);
    k_work_schedule(&trackpad_led_work, POLL_INTERVAL);

    LOG_INF("Trackpad LED polling initialized");
    return 0;
}

SYS_INIT(trackpad_led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
