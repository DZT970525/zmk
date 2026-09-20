/*
 * custom_led.c - 分体连接状态指示 LED
 *
 * 逻辑：
 *   - 若 zmk_split_bt_peripheral_is_connected() == true，则 LED 常灭
 *   - 若未连接，每 1 秒闪烁 50 ms
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>

#include <zmk/split/bluetooth/peripheral.h>
#if __has_include(<zmk/split/central.h>)
#include <zmk/split/central.h>
#else
#include <zmk/split/bluetooth/central.h>
#endif

LOG_MODULE_REGISTER(custom_led, CONFIG_ZMK_LOG_LEVEL);

/* ==== Devicetree chosen 节点 ==== */
#if !DT_HAS_CHOSEN(zmk_custom_led)
#error "Missing chosen node: zmk,custom_led"
#endif

static const struct device *led_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_custom_led));

/* ==== 配置参数 ==== */
#define DISCONNECT_PERIOD_MS 1000 /* 间隔 1 秒 */
#define DISCONNECT_FLASH_MS 50    /* 亮 50 ms */
#define LED_MAX_BRT 20            /* 常亮/闪烁最大亮度 */

static struct k_work_delayable flash_work;
static bool flash_on = false;

/* ==== 设置 LED 亮度 ==== */
static void set_led_brightness(int brt) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED device not ready");
        return;
    }
    if (brt < 0)
        brt = 0;
    if (brt > 100)
        brt = 100;
    led_set_brightness(led_dev, 0, brt);
}

/* ==== 闪烁处理 ==== */
static void flash_work_handler(struct k_work *work) {
    /* 如果分体已连接 -> LED 常灭 */
    if (zmk_split_bt_peripheral_is_connected()) {
        set_led_brightness(0);
        /* 继续以 1 秒周期检测连接状态 */
        k_work_reschedule(&flash_work, K_MSEC(DISCONNECT_PERIOD_MS));
        flash_on = false;
        return;
    }

    /* 未连接 -> 每秒闪 50 ms */
    if (!flash_on) {
        /* 点亮 LED */
        set_led_brightness(LED_MAX_BRT);
        flash_on = true;
        /* 50 ms 后关闭 LED */
        k_work_reschedule(&flash_work, K_MSEC(DISCONNECT_FLASH_MS));
    } else {
        /* 熄灭 LED，等待下一次 1 秒周期 */
        set_led_brightness(0);
        flash_on = false;
        k_work_reschedule(&flash_work, K_MSEC(DISCONNECT_PERIOD_MS - DISCONNECT_FLASH_MS));
    }
}

/* ==== 初始化 ==== */
static int custom_led_init(void) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED device not ready");
        return -ENODEV;
    }

    set_led_brightness(0);
    k_work_init_delayable(&flash_work, flash_work_handler);
    /* 立即启动，每秒闪逻辑从这里开始 */
    k_work_schedule(&flash_work, K_MSEC(DISCONNECT_PERIOD_MS));

    LOG_INF("Custom LED split-disconnect indicator initialized");
    return 0;
}

/* ==== 系统初始化 ==== */
SYS_INIT(custom_led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
