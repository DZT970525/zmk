#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <zmk/hid_indicators.h>

#define HID_INDICATORS_CAPS_LOCK (1 << 1)

// === 配置参数 ===
#define POLLING_INTERVAL_MS 10
#define SCROLL_INTERVAL_MS 50
#define SMOOTHING_SIZE 2

LOG_MODULE_REGISTER(a320_input_handler, LOG_LEVEL_DBG);

// === 全局变量 ===
static const struct device *a320_sensor;

void a320_thread_main(void *arg1, void *arg2, void *arg3) {
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    struct sensor_value xy_pos;

    while (1) {

        if (sensor_channel_get(a320_sensor, SENSOR_CHAN_AMBIENT_TEMP, &xy_pos) == 0) {
            int8_t rawx = xy_pos.val2;
            int8_t rawy = xy_pos.val1;

            bool capslock = (zmk_hid_indicators_get_current_profile() & HID_INDICATORS_CAPS_LOCK);

            if (!capslock) {
                float tp_factor = 0.8f + 0.01f;
                rawx = ((rawx < 127) ? rawx : rawx - 256) * 3 / 2 * tp_factor;
                rawy = ((rawy < 127) ? rawy : rawy - 256) * 3 / 2 * tp_factor;
            }

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
        }

        k_sleep(K_MSEC(POLLING_INTERVAL_MS));
    }
}

// === 初始化 ===

K_THREAD_STACK_DEFINE(a320_thread_stack, 1024);
static struct k_thread a320_thread_data;

static int a320_input_handler_init(void) {
    LOG_INF("Initializing A320 input handler...");

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
