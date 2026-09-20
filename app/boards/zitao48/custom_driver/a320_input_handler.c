#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zmk/hid.h>
#include "a320_input_handler.h"
#include <zmk/endpoints.h>
#include <zmk/hid_indicators.h>
#include <zmk/indicator_tp.h>

#define HID_INDICATORS_CAPS_LOCK (1 << 1)

// === 配置参数 ===
#define MOTION_GPIO_NODE DT_NODELABEL(gpio0)
#define MOTION_GPIO_PIN 2
#define POLLING_INTERVAL_MS 10
#define SCROLL_INTERVAL_MS 50
#define SMOOTHING_SIZE 2

LOG_MODULE_REGISTER(a320_input_handler, LOG_LEVEL_DBG);

// === 全局变量 ===
static const struct device *motion_gpio_dev;
static const struct device *a320_sensor;

static bool touched = false;

// 滑动平均缓冲
static int8_t dx_buffer[SMOOTHING_SIZE] = {0};
static int8_t dy_buffer[SMOOTHING_SIZE] = {0};
static uint8_t buffer_index = 0;

// GPIO 边沿收集逻辑
static bool last_state = true;
static uint32_t last_fall_time = 0;
static uint8_t collect_count = 0;
static bool collecting = false;

static void apply_smoothing(int8_t *x, int8_t *y) {
    dx_buffer[buffer_index] = *x;
    dy_buffer[buffer_index] = *y;

    int32_t sum_x = 0, sum_y = 0;
    for (int i = 0; i < SMOOTHING_SIZE; i++) {
        sum_x += dx_buffer[i];
        sum_y += dy_buffer[i];
    }

    *x = sum_x / SMOOTHING_SIZE;
    *y = sum_y / SMOOTHING_SIZE;

    buffer_index = (buffer_index + 1) % SMOOTHING_SIZE;
}

void a320_thread_main(void *arg1, void *arg2, void *arg3) {
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    struct sensor_value xy_pos;

    while (1) {
        int pin_state = gpio_pin_get(motion_gpio_dev, MOTION_GPIO_PIN);
        uint32_t now = k_uptime_get_32();

        // 检测下降沿
        if (last_state && pin_state == 0) {
            uint32_t interval = now - last_fall_time;
            if (interval > 200) {
                collect_count = 0;
            } else if (interval < 30) {
                collect_count++;
            }
            last_fall_time = now;
            collecting = true;
        }
        last_state = pin_state;

        if (collecting && pin_state != 0) {
            collecting = false;
        }

        if (pin_state == 0 &&
            sensor_channel_get(a320_sensor, SENSOR_CHAN_AMBIENT_TEMP, &xy_pos) == 0) {
            int8_t rawx = xy_pos.val2;
            int8_t rawy = xy_pos.val1;

            bool capslock = (zmk_hid_indicators_get_current_profile() & HID_INDICATORS_CAPS_LOCK);

            if (!capslock) {
                uint8_t tp_led_brt = indicator_tp_get_last_valid_brightness();
                float tp_factor = 0.4f + 0.01f * tp_led_brt;
                rawx = ((rawx < 127) ? rawx : rawx - 256) * 3 / 2 * tp_factor;
                rawy = ((rawy < 127) ? rawy : rawy - 256) * 3 / 2 * tp_factor;

                if (collecting && collect_count < 2) {
                    rawx /= 3;
                    rawy /= 3;
                }
            }

            apply_smoothing(&rawx, &rawy);

            if (capslock) {
                int8_t x = -rawx;
                int8_t y = rawy;
                int8_t scroll_x = 0, scroll_y = 0;

                if (abs(y) >= 128) {
                    scroll_x = -x / 24;
                    scroll_y = -y / 24;
                } else if (abs(y) >= 64) {
                    scroll_x = -x / 16;
                    scroll_y = -y / 16;
                } else if (abs(y) >= 32) {
                    scroll_x = -x / 12;
                    scroll_y = -y / 12;
                } else if (abs(y) >= 21) {
                    scroll_x = -x / 8;
                    scroll_y = -y / 8;
                } else if (abs(y) >= 3) {
                    scroll_x = (x > 0) ? -1 : (x < 0) ? 1 : 0;
                    scroll_y = (y > 0) ? -1 : (y < 0) ? 1 : 0;
                } else {
                    scroll_x = (x > 0) ? -1 : (x < 0) ? 1 : 0;
                    scroll_y = 0;
                }

                zmk_hid_mouse_movement_set(0, 0); // 防止移动
                zmk_hid_mouse_scroll_set(0, 0);
                zmk_hid_mouse_scroll_update(scroll_x, scroll_y);
                zmk_endpoints_send_mouse_report();

                k_sleep(K_MSEC(SCROLL_INTERVAL_MS));
            } else {
                zmk_hid_mouse_scroll_set(0, 0);
                zmk_hid_mouse_movement_set(0, 0);
                zmk_hid_mouse_movement_update(rawx, rawy);
                zmk_endpoints_send_mouse_report();
            }

            touched = true;
        } else {
            // === 触控板未被触摸，清空鼠标状态，避免后续误触 ===
            zmk_hid_mouse_scroll_set(0, 0);
            zmk_hid_mouse_movement_set(0, 0);
            zmk_endpoints_send_mouse_report();
            touched = false;
        }

        // k_sleep(K_MSEC(POLLING_INTERVAL_MS));
    }
}

bool tp_is_touched(void) { return touched; }
// === 初始化 ===

K_THREAD_STACK_DEFINE(a320_thread_stack, 1024);
static struct k_thread a320_thread_data;

static int a320_input_handler_init(void) {
    LOG_INF("Initializing A320 input handler...");

    motion_gpio_dev = DEVICE_DT_GET(MOTION_GPIO_NODE);
    if (!device_is_ready(motion_gpio_dev)) {
        LOG_ERR("Motion GPIO device not ready");
        return -ENODEV;
    }
    gpio_pin_configure(motion_gpio_dev, MOTION_GPIO_PIN, GPIO_INPUT | GPIO_PULL_UP);

    a320_sensor = DEVICE_DT_GET_ANY(avago_a320);
    if (!device_is_ready(a320_sensor)) {
        LOG_ERR("A320 sensor not ready");
        return -ENODEV;
    }

    k_thread_create(&a320_thread_data, a320_thread_stack, K_THREAD_STACK_SIZEOF(a320_thread_stack),
                    a320_thread_main, NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);

    LOG_INF("A320 input handler thread started.");
    return 0;
}

SYS_INIT(a320_input_handler_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
