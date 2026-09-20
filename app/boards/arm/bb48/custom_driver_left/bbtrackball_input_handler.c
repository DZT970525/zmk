/*
 * bbtrackball_input_handler.c - BB Trackball with GPIO interrupt + periodic report
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <math.h>
#include <stdlib.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>

#include "bbtrackball_input_handler.h"
#include "trackball_led.h"

LOG_MODULE_REGISTER(bbtrackball_input_handler, LOG_LEVEL_INF);

/* ==== GPIO Pins ==== */
#define DOWN_GPIO_PIN 14
#define LEFT_GPIO_PIN 8
#define UP_GPIO_PIN 12
#define RIGHT_GPIO_PIN 9

#define GPIO0_DEV DT_NODELABEL(gpio0)
#define GPIO1_DEV DT_NODELABEL(gpio1)

/* ==== Config ==== */
#define BASE_MOVE_PIXELS 3
#define EXPONENTIAL_BASE 1.12f
#define SPEED_SCALE 60.0f
#define REPORT_INTERVAL_MS 10

static bool moved = false;

/* ==== Direction Struct ==== */
typedef struct {
    const struct device *gpio_dev;
    int pin;
    int last_state;
    uint32_t last_time;
    int sign; /* -1 or +1 */
} DirInput;

static DirInput dir_inputs[] = {
    {DEVICE_DT_GET(GPIO0_DEV), LEFT_GPIO_PIN, 1, 0, -1},
    {DEVICE_DT_GET(GPIO1_DEV), RIGHT_GPIO_PIN, 1, 0, +1},
    {DEVICE_DT_GET(GPIO0_DEV), UP_GPIO_PIN, 1, 0, -1},
    {DEVICE_DT_GET(GPIO0_DEV), DOWN_GPIO_PIN, 1, 0, +1},
};

static int dx_acc = 0;
static int dy_acc = 0;
static struct gpio_callback gpio_cbs[ARRAY_SIZE(dir_inputs)];
static struct k_work_delayable report_work;

/* ==== 外部接口 ==== */
bool trackball_is_moving(void) { return moved; }

/* ==== 中断回调 ==== */
static void dir_edge_cb(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    for (size_t i = 0; i < ARRAY_SIZE(dir_inputs); i++) {
        DirInput *d = &dir_inputs[i];
        if ((dev == d->gpio_dev) && (pins & BIT(d->pin))) {
            int val = gpio_pin_get(dev, d->pin);
            if (val != d->last_state) {
                uint32_t now = k_uptime_get_32();
                uint32_t delta = now - d->last_time;
                if (delta == 0)
                    delta = 1;

                float speed_factor = SPEED_SCALE / (float)delta;
                float mult = powf(EXPONENTIAL_BASE, speed_factor);
                int delta_px = (int)roundf(BASE_MOVE_PIXELS * mult);

                /* X 方向 inputs[0]/[1], Y 方向 inputs[2]/[3] */
                if (i < 2) {
                    dx_acc += d->sign * delta_px;
                } else {
                    dy_acc += d->sign * delta_px;
                }

                d->last_state = val;
                d->last_time = now;
            }
        }
    }
}

/* ==== HID 报告定时任务 ==== */
static void report_work_handler(struct k_work *work) {
    if (dx_acc || dy_acc) {
        int current_layer = zmk_keymap_highest_layer_active();
        moved = true;

        if (current_layer == 0) {
            uint8_t trackball_led_brt = trackball_led_get_last_valid_brightness();
            float trackball_factor = 0.4f + 0.01f * trackball_led_brt;
            int dx = -1 * (int)(dx_acc * 1.5f * trackball_factor);
            int dy = -1 * (int)(dy_acc * 1.5f * trackball_factor);

            zmk_hid_mouse_scroll_set(0, 0);
            zmk_hid_mouse_movement_set(0, 0);
            zmk_hid_mouse_movement_update(dx, dy);
            zmk_endpoints_send_mouse_report();
        } else if (current_layer == 1) {
            int16_t scroll_x = (int16_t)(copysign(fmax(1.0, floor(fabs(dx_acc))), dx_acc)) / 2;
            int16_t scroll_y =
                -1 * (int16_t)(-copysign(fmax(1.0, floor(fabs(dy_acc))), dy_acc)) / 2;

            zmk_hid_mouse_movement_set(0, 0);
            zmk_hid_mouse_scroll_set(0, 0);
            zmk_hid_mouse_scroll_update(scroll_x, scroll_y);
            zmk_endpoints_send_mouse_report();
            k_sleep(K_MSEC(20));
        }

        dx_acc = 0;
        dy_acc = 0;
    } else {
        /* 空闲也发一次，保证释放状态 */
        zmk_hid_mouse_scroll_set(0, 0);
        zmk_hid_mouse_movement_set(0, 0);
        zmk_hid_mouse_movement_update(0, 0);
        zmk_endpoints_send_mouse_report();
        moved = false;
    }

    /* 重新调度下一次 */
    k_work_schedule(&report_work, K_MSEC(REPORT_INTERVAL_MS));
}

/* ==== 初始化 ==== */
static int bbtrackball_init(void) {
    LOG_INF("Initializing BBtrackball (interrupt + workqueue mode)...");

    for (size_t i = 0; i < ARRAY_SIZE(dir_inputs); i++) {
        DirInput *d = &dir_inputs[i];
        gpio_pin_configure(d->gpio_dev, d->pin, GPIO_INPUT | GPIO_PULL_UP | GPIO_INT_EDGE_BOTH);
        d->last_state = gpio_pin_get(d->gpio_dev, d->pin);
        d->last_time = k_uptime_get_32();

        gpio_init_callback(&gpio_cbs[i], dir_edge_cb, BIT(d->pin));
        gpio_add_callback(d->gpio_dev, &gpio_cbs[i]);
        gpio_pin_interrupt_configure(d->gpio_dev, d->pin, GPIO_INT_EDGE_BOTH);
    }

    k_work_init_delayable(&report_work, report_work_handler);
    k_work_schedule(&report_work, K_MSEC(REPORT_INTERVAL_MS));
    return 0;
}

/* 在应用启动阶段自动执行初始化 */
SYS_INIT(bbtrackball_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
