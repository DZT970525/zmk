/*
 * Synaptics S3501 Touchpad driver for ZMK (polling mode, with acceleration)
 * Ported from STM8L driver by TinLethax, 2022
 * Adapted for Zephyr by ZitaoTech, 2025
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT synaptics_s3501

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <zmk/hid.h>
#include <math.h>

LOG_MODULE_REGISTER(s3501trackpad, CONFIG_ZMK_LOG_LEVEL);

#define S3501_ADDR 0x20
#define RMI4_F01 0x01
#define RMI4_F12 0x12
#define RMI4_F1A 0x1A

#define S3501_REPORT_LEN 88
#define S3501_POLL_INTERVAL_MS 10           /* 100Hz 采样率 */
#define S3501_CONTINUOUS_THRESHOLD_MS 20    /* 连续移动阈值 */
#define S3501_PRESSURE_IGNORE_THRESHOLD 200 /* 压力超过此值不参与计算 */

/* ==== 加速度配置 ==== */
#define BASE_MOVE_PIXELS 0.1f
#define EXPONENTIAL_BASE 1.16f
#define SPEED_SCALE 100.0f

/* I2C 设备 */
static const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));

static uint8_t current_page = 0;
static uint8_t F12_report_addr = 0x06;
static uint8_t report[S3501_REPORT_LEN];
static struct k_work_delayable poll_work;

/* 单指位置与时间戳 */
static uint16_t last_x = 0, last_y = 0;
static uint32_t last_time = 0;
static bool has_last = false;

/* 双指滚动参考值 */
static uint16_t last_y2 = 0;
static bool has_two_fingers = false;

/* 切换 page */
static int s3501_set_page(uint8_t page) {
    if (page == current_page)
        return 0;

    uint8_t buf[2] = {0xFF, page};
    int ret = i2c_write(i2c_dev, buf, 2, S3501_ADDR);
    if (ret < 0) {
        LOG_ERR("Failed to set page %d", page);
        return ret;
    }
    current_page = page;
    return 0;
}

/* 从指定地址读取数据 */
static int s3501_read(uint8_t addr, uint8_t *data, uint8_t len) {
    return i2c_write_read(i2c_dev, S3501_ADDR, &addr, 1, data, len);
}

/* 主轮询函数 */
static void s3501_poll_fn(struct k_work *work) {
    ARG_UNUSED(work);

    uint32_t now = k_uptime_get_32();
    int ret = s3501_set_page(0);
    if (ret < 0)
        goto reschedule;

    ret = s3501_read(F12_report_addr, report, S3501_REPORT_LEN);
    if (ret < 0) {
        LOG_ERR("I2C read failed: %d", ret);
        goto reschedule;
    }

    bool finger1 = report[0];
    bool finger2 = report[8];

    if (finger1 && !finger2) {
        /* ==== 单指移动模式 ==== */
        uint16_t x = (report[2] << 8) | report[1];
        uint16_t y = (report[4] << 8) | report[3];
        uint8_t pressure = report[5];

        /* --- 压力过滤 --- */
        if (pressure > S3501_PRESSURE_IGNORE_THRESHOLD && has_last) {
            x = last_x;
            y = last_y;
            LOG_DBG("Pressure=%d ignored", pressure);
        }

        if (has_last) {
            uint32_t dt = now - last_time;
            if (dt < 1)
                dt = 1;

            if (dt < S3501_CONTINUOUS_THRESHOLD_MS) {
                int16_t dx = (int16_t)x - (int16_t)last_x;
                int16_t dy = (int16_t)y - (int16_t)last_y;

                /* ==== 加速度逻辑 ==== */
                float speed_factor = SPEED_SCALE / (float)dt;
                float mult = powf(EXPONENTIAL_BASE, speed_factor);
                float dx_f = dx * mult * BASE_MOVE_PIXELS;
                float dy_f = dy * mult * BASE_MOVE_PIXELS;

                int16_t dx_acc = (int16_t)roundf(dx_f);
                int16_t dy_acc = (int16_t)roundf(dy_f);

                LOG_INF("Move dx=%d dy=%d (accel %.2fx, dt=%dms, P=%d)", dx_acc, dy_acc, mult, dt,
                        pressure);

                zmk_hid_mouse_scroll_set(0, 0);
                zmk_hid_mouse_movement_set(0, 0);
                zmk_hid_mouse_movement_update(dx_acc, dy_acc);
                zmk_endpoints_send_mouse_report();
            } else {
                LOG_INF("New single touch (gap=%dms)", dt);
            }
        }

        last_x = x;
        last_y = y;
        last_time = now;
        has_last = true;
        has_two_fingers = false;

    } else if (finger1 && finger2) {
        /* ==== 双指滚动模式 ==== */
        uint16_t y1 = (report[4] << 8) | report[3];
        uint16_t y2 = (report[12] << 8) | report[11];
        uint8_t p1 = report[5];
        uint8_t p2 = report[13];

        /* --- 压力过滤 --- */
        if ((p1 > S3501_PRESSURE_IGNORE_THRESHOLD) || (p2 > S3501_PRESSURE_IGNORE_THRESHOLD)) {
            y1 = last_y2;
            y2 = last_y2;
            LOG_DBG("Pressure filtered (p1=%d p2=%d)", p1, p2);
        }

        uint16_t avg_y = (y1 + y2) / 2;

        if (has_two_fingers) {
            int16_t dy = (int16_t)avg_y - (int16_t)last_y2;
            int16_t scroll_y = -dy / 5;
            int16_t scroll_x = 0;

            if (scroll_y != 0) {
                LOG_INF("Scroll dy=%d -> scroll_y=%d", dy, scroll_y);
                zmk_hid_mouse_movement_set(0, 0);
                zmk_hid_mouse_scroll_set(0, 0);
                zmk_hid_mouse_scroll_update(scroll_x, scroll_y);
                zmk_endpoints_send_mouse_report();
            }
        } else {
            LOG_INF("Two-finger scroll start");
        }

        last_y2 = avg_y;
        has_two_fingers = true;
        has_last = false;

    } else {
        /* 没有手指 */
        has_last = false;
        has_two_fingers = false;
    }

reschedule:
    k_work_schedule(&poll_work, K_MSEC(S3501_POLL_INTERVAL_MS));
}

/* 初始化 */
static int s3501_init(const struct device *dev) {
    ARG_UNUSED(dev);

    if (!device_is_ready(i2c_dev)) {
        LOG_ERR("I2C device not ready");
        return -ENODEV;
    }

    LOG_INF("S3501 trackpad init (100Hz polling, accel enabled)");

    s3501_set_page(0);
    k_work_init_delayable(&poll_work, s3501_poll_fn);
    k_work_schedule(&poll_work, K_MSEC(S3501_POLL_INTERVAL_MS));

    return 0;
}

DEVICE_DT_INST_DEFINE(0, s3501_init, NULL, NULL, NULL, APPLICATION, 90, NULL);
