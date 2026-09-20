/*
 * custom_led.c - 根据连接方式和 Layer 控制 LED
 *
 * 优先级：
 *   1. USB (ZMK_TRANSPORT_USB)：LED 每秒闪一次，每次亮 100 ms
 *   2. BLE (ZMK_TRANSPORT_BLE)：进入层控制
 *   3. 其他：LED 关闭
 *
 * 层控制规则：
 *   - Layer 0: LED 关闭
 *   - Layer 1: LED 常亮
 *   - Layer 2: LED 呼吸动画（渐亮渐暗）
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>

LOG_MODULE_REGISTER(custom_led, CONFIG_ZMK_LOG_LEVEL);

/* ==== Devicetree chosen 节点 ==== */
#if !DT_HAS_CHOSEN(zmk_custom_led)
#error "Missing chosen node: zmk,custom_led"
#endif

static const struct device *led_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_custom_led));

/* ==== 配置参数 ==== */
#define FLASH_ON_MS 100    /* USB 模式闪烁亮灯时间 */
#define FLASH_PERIOD 1000  /* USB 模式闪烁周期 1 秒 */
#define LED_MAX_BRT 20     /* 常亮/闪烁最大亮度 */
#define BREATH_STEP 5      /* 呼吸渐变步进 */
#define BREATH_INTERVAL 30 /* 呼吸动画间隔 */

/* ==== 内部变量 ==== */
static struct k_work_delayable led_work;
static bool usb_flash_on = false;
static int breath_brt = 0;
static bool breath_up = true;

/* ==== 设置 LED 亮度 ==== */
static void set_led_brightness(int brt) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED device not ready");
        return;
    }
    if (brt < 0)
        brt = 0;
    if (brt > LED_MAX_BRT)
        brt = LED_MAX_BRT;

    led_set_brightness(led_dev, 0, brt);
}

/* ==== 主调度处理函数 ==== */
static void led_work_handler(struct k_work *work) {
    enum zmk_transport transport = zmk_endpoints_selected().transport;

    /* --- 1. USB 优先级最高 --- */
    if (transport == ZMK_TRANSPORT_USB) {
        if (!usb_flash_on) {
            set_led_brightness(LED_MAX_BRT);
            usb_flash_on = true;
            k_work_reschedule(&led_work, K_MSEC(FLASH_ON_MS));
        } else {
            set_led_brightness(0);
            usb_flash_on = false;
            k_work_reschedule(&led_work, K_MSEC(FLASH_PERIOD - FLASH_ON_MS));
        }
        return; /* 已处理，直接返回 */
    }

    /* --- 2. BLE 情况：进入层逻辑 --- */
    if (transport == ZMK_TRANSPORT_BLE) {
        int current_layer = zmk_keymap_highest_layer_active();
        switch (current_layer) {
        case 0:
            set_led_brightness(0);
            k_work_reschedule(&led_work, K_MSEC(BREATH_INTERVAL));
            break;

        case 1:
            set_led_brightness(LED_MAX_BRT);
            k_work_reschedule(&led_work, K_MSEC(BREATH_INTERVAL));
            break;

        case 2:
            /* 呼吸渐亮渐暗动画 */
            set_led_brightness(breath_brt);

            if (breath_up) {
                breath_brt += BREATH_STEP;
                if (breath_brt >= LED_MAX_BRT) {
                    breath_brt = LED_MAX_BRT;
                    breath_up = false;
                }
            } else {
                breath_brt -= BREATH_STEP;
                if (breath_brt <= 0) {
                    breath_brt = 0;
                    breath_up = true;
                }
            }
            k_work_reschedule(&led_work, K_MSEC(BREATH_INTERVAL));
            break;

        default:
            set_led_brightness(0);
            k_work_reschedule(&led_work, K_MSEC(BREATH_INTERVAL));
            break;
        }
        return;
    }

    /* --- 3. 其他情况 --- */
    set_led_brightness(0);
    usb_flash_on = false;
    k_work_reschedule(&led_work, K_MSEC(FLASH_PERIOD));
}

/* ==== 初始化 ==== */
static int custom_led_init(void) {
    if (!device_is_ready(led_dev)) {
        LOG_ERR("LED device not ready");
        return -ENODEV;
    }

    usb_flash_on = false;
    breath_brt = 0;
    breath_up = true;
    set_led_brightness(0);

    k_work_init_delayable(&led_work, led_work_handler);
    /* 初始调度 */
    k_work_schedule(&led_work, K_MSEC(FLASH_PERIOD));

    LOG_INF("Custom LED with transport+layer control initialized");
    return 0;
}

/* ==== SYS_INIT 调用 ==== */
SYS_INIT(custom_led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
