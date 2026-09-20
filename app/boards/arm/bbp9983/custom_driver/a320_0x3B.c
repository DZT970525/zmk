#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <zmk/hid_indicators.h>

#define HID_INDICATORS_CAPS_LOCK (1 << 1)

LOG_MODULE_REGISTER(a320_0x3B, LOG_LEVEL_INF);

// === set parameter ===
#define POLLING_INTERVAL_MS 5
#define SMOOTHING_SIZE 2
#define SCROLL_INTERVAL_MS CONFIG_A320_TRACKPAD_SCROLL_INTERVAL

/* ==== I2C Device ==== */
#define A320_NODE DT_INST(0, avago_a320)
static const struct i2c_dt_spec a320_i2c = I2C_DT_SPEC_GET(A320_NODE);

/* ====initial ==== */
static int a320_init(void) {
    LOG_INF("Initializing A320 input handler...");

    if (!device_is_ready(a320_i2c.bus)) {
        LOG_ERR("I2C bus not ready for A320 sensor");
        return -ENODEV;
    }

    LOG_INF("A320 sensor initialized at addr=0x%02x", a320_i2c.addr);
    return 0;
}

/* ==== read motion data ==== */
static int a320_read_motion(int16_t *dx, int16_t *dy) {

    uint8_t buf[3] = {0};

    /* first write to 0x82 */
    uint8_t reg = 0x82;
    int ret = i2c_write_dt(&a320_i2c, &reg, 1);
    if (ret < 0) {
        LOG_ERR("Failed to write register address 0x82: %d", ret);
        return ret;
    }

    /* read 7 byte from 0x82  */
    ret = i2c_burst_read_dt(&a320_i2c, 0x82, buf, sizeof(buf));
    if (ret < 0) {
        LOG_ERR("Failed to read from 0x82: %d", ret);
        return ret;
    }

    /* the second and the furth byte is dx/dy */
    *dx = (int8_t)buf[1];
    *dy = (int8_t)buf[2];

    return 0;
}

/* ==== thread reading ==== */
void a320_polling_thread(void) {
    int16_t dx, dy;

    if (a320_init() < 0) {
        return;
    }

    while (1) {
        if (a320_read_motion(&dx, &dy) == 0) {
            if (dx || dy) {
                bool capslock =
                    (zmk_hid_indicators_get_current_profile() & HID_INDICATORS_CAPS_LOCK);
                LOG_INF("Motion dx=%d dy=%d", dx, dy);
                if (!capslock) {
                    dx = dx * 3 / 2 * CONFIG_A320_TRACKPAD_SPEEDMULTIPLIER_HORIZONTAL / 100;
                    dy = -1 * dy * 3 / 2 * CONFIG_A320_TRACKPAD_SPEEDMULTIPLIER_VERTICAL / 100;
                }
                if (capslock) {
                    int8_t x = -dx;
                    int8_t y = dy;
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

                    zmk_hid_mouse_movement_set(0, 0);
                    zmk_hid_mouse_scroll_set(0, 0);
                    zmk_hid_mouse_scroll_update(scroll_x, scroll_y);
                    zmk_endpoints_send_mouse_report();

                    k_msleep(SCROLL_INTERVAL_MS);
                } else {
                    /* report mouse hid */
                    zmk_hid_mouse_scroll_set(0, 0);
                    zmk_hid_mouse_movement_set(0, 0);
                    zmk_hid_mouse_movement_update(dx, dy);
                    zmk_endpoints_send_mouse_report();
                }
            }
        }

        k_msleep(POLLING_INTERVAL_MS); /* 10ms  */
    }
}

/* ==== start thread ==== */
K_THREAD_DEFINE(a320_thread_id, 1024, a320_polling_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0,
                0);
