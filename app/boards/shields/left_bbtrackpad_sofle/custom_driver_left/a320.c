/*
 * A320 trackpad HID over I2C Driver (Zephyr Input Subsystem)
 * Interrupt-driven version (minimal modification)
 * Copyright (c) 2025 ZitaoTech
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT avago_a320

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <stdlib.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <math.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zmk/hid_indicators.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <dt-bindings/zmk/modifiers.h>

#include "trackpad_led.h"
#include "a320.h"
#include "pointing_dpi.h"
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
#include <zitaotech/zmk/trackpad.h>
#endif

LOG_MODULE_REGISTER(a320, CONFIG_A320_LOG_LEVEL);

/* ========= ⭐ A320 专用 Work Queue ========= */
#define A320_WORKQ_STACK_SIZE 2048
#define A320_WORKQ_PRIORITY 5

/* ========= ⭐ NEW: I2C Mutex ========= */
static struct k_mutex a320_i2c_mutex;

K_THREAD_STACK_DEFINE(a320_workq_stack, A320_WORKQ_STACK_SIZE);
static struct k_work_q a320_workq;

/* ========================================================================= */
/* 鼠标与滚轮可调参数 (已映射至 Kconfig，用户可在 .conf 中配置)                 */
/* ========================================================================= */

// --- 滚轮方向配置 ---
#define SCROLL_X_DIR (-CONFIG_A320_SCROLL_X_DIR)
#define SCROLL_Y_DIR CONFIG_A320_SCROLL_Y_DIR

// --- 鼠标指针基础配置 (Kconfig 为整数百分比，这里除以 100 转为浮点数) ---
#define MOUSE_BASE_SPEED (CONFIG_A320_MOUSE_BASE_SPEED_PERCENT / 100.0f)
#define MOUSE_SENS_BASE (CONFIG_A320_MOUSE_SENS_BASE_PERCENT / 100.0f)
#define MOUSE_SENS_STEP (CONFIG_A320_MOUSE_SENS_STEP_PERCENT / 100.0f)

/* ---------------------------------------------------------------------------
 * Runtime trackpad settings (ZitaoTech Studio "Trackpad" tab).
 *
 * DPI comes from the zmk-feature-zitaotech-trackpad module when it is enabled;
 * the helpers below also fold the module's user-chosen inversion, scroll speed
 * and burst multiplier into the driver's compile-time defaults.  With the
 * module disabled the driver behaves exactly as before, which is what the
 * right half and the other shields using this file keep doing.
 * ------------------------------------------------------------------------ */
static inline int trackpad_scroll_x_sign(void) {
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    return zitaotech_trackpad_scroll_invert_x() ? -1 : 1;
#else
    return 1;
#endif
}

static inline int trackpad_scroll_y_sign(void) {
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    return zitaotech_trackpad_scroll_invert_y() ? -1 : 1;
#else
    return 1;
#endif
}

static inline float trackpad_scroll_speed_scale(void) {
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    return (float)zitaotech_trackpad_scroll_speed_percent() / 100.0f;
#else
    return 1.0f;
#endif
}

/* Burst mode: x1..x10 on top of the DPI-derived pointer speed. */
static inline float trackpad_pointer_burst_scale(void) {
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    return (float)zitaotech_trackpad_pointer_multiplier();
#else
    return 1.0f;
#endif
}

/* Direction-mode swipe threshold; user-tunable from ZitaoTech Studio. */
static inline int32_t trackpad_arrow_swipe_threshold(void) {
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    return (int32_t)zitaotech_trackpad_arrow_swipe_threshold();
#else
    return ARROW_SWIPE_THRESHOLD;
#endif
}

/* Mode keys: the positions that switch the trackpad's behaviour while held.
 * They are user-assignable from ZitaoTech Studio; -1 means "not assigned". */
static inline int32_t trackpad_arrow_key_position(void) {
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    return zitaotech_trackpad_arrow_key_position();
#else
    return ARROW_KEY_POSITION;
#endif
}

static inline int32_t trackpad_scroll_key_position(void) {
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    return zitaotech_trackpad_scroll_key_position();
#else
    return SCROLL_KEY_POSITION;
#endif
}

static inline int32_t trackpad_fast_key_position(void) {
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    return zitaotech_trackpad_fast_key_position();
#else
    return -1;
#endif
}

static inline int32_t trackpad_slow_key_position(void) {
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    return zitaotech_trackpad_slow_key_position();
#else
    return -1;
#endif
}

/* ========= Motion GPIO ========= */

#define MOTION_GPIO_NODE DT_NODELABEL(gpio0)
#define MOTION_GPIO_PIN 5
#define MOTION_GPIO_FLAGS (GPIO_ACTIVE_LOW | GPIO_PULL_UP)

/* ========= A320 常量 ========= */
#define A320_I2C_ADDR_3B 0x3B
#define A320_I2C_ADDR_37 0x37
#define A320_DEFAULT_I2C_ADDR A320_I2C_ADDR_37
#define A320_READ_RETRY_MS 20
#define A320_37_SAMPLE_MS 5
#define MOTION_AVERAGE_SAMPLES 3
#define MOTION_AVERAGE_RESET_MS 50

#define ARROW_KEY_POSITION 61
#define SCROLL_KEY_POSITION 0
#define ARROW_SWIPE_THRESHOLD 34
#define ARROW_GESTURE_RESET_MS 120
#define TOUCH_IDLE_TIMEOUT 50 // 30~80ms 看手感
/* ========= Watch Dog ========= */
static uint32_t last_activity_time = 0;
#define A320_WDT_TIMEOUT 200
/* ========= 全局状态 ========= */
static atomic_t scroll_key_pressed = ATOMIC_INIT(0);
static atomic_t fast_key_pressed = ATOMIC_INIT(0);
static atomic_t slow_key_pressed = ATOMIC_INIT(0);
static atomic_t arrow_key_state = ATOMIC_INIT(0);
uint32_t last_packet_time = 0;
static bool touched = false;

/*
 * Which mode the trackpad is in *right now*.
 *
 * The direction/pointer mode key inverts the base mode from Studio (see
 * zitaotech_trackpad_mode()) for as long as it is held, so the same key is
 * "direction mode" on a pointer-mode keyboard and "pointer mode" on a
 * direction-mode one.
 *
 * The returned token packs everything that must invalidate a swipe tap waiting
 * in the queue: bit 0 is the effective arrow mode (the LED/scroll paths and the
 * arrow-tap check all read it that way), bit 1 is the base mode, and the higher
 * bits are the mode key's edge counter.
 */
static atomic_val_t arrow_mode_token(void) {
    const atomic_val_t raw = atomic_get(&arrow_key_state);
    bool base_arrow = false;
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    base_arrow = zitaotech_trackpad_mode() == ZITAOTECH_TRACKPAD_MODE_ARROW;
#endif
    const bool arrow = ((raw & 1) != 0) != base_arrow;

    return ((raw >> 1) << 2) | (base_arrow ? 2 : 0) | (arrow ? 1 : 0);
}

#define HID_INDICATORS_CAPS_LOCK (1 << 1)

/* ========= Scroll / Fast / Slow / Arrow 按键监听 ========= */
static int special_key_listener_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (!ev) {
        return 0;
    }

    if (ev->position == (uint32_t)trackpad_fast_key_position()) {
        atomic_set(&fast_key_pressed, ev->state);
    } else if (ev->position == (uint32_t)trackpad_slow_key_position()) {
        atomic_set(&slow_key_pressed, ev->state);
    } else if (ev->position == (uint32_t)trackpad_scroll_key_position()) {
        atomic_set(&scroll_key_pressed, ev->state);
    } else if (ev->position == (uint32_t)trackpad_arrow_key_position()) {
        /* Odd = held, even = released; each edge also invalidates queued
         * gestures, and bit 0 of the raw counter is the key's own state. */
        if (((atomic_get(&arrow_key_state) & 1) != 0) != ev->state) {
            atomic_inc(&arrow_key_state);
        }
    }

    return 0;
}
ZMK_LISTENER(a320_special_key_listener, special_key_listener_cb);
ZMK_SUBSCRIPTION(a320_special_key_listener, zmk_position_state_changed);

struct a320_config {
    struct i2c_dt_spec i2c;
    struct gpio_dt_spec motion_gpio;
};

typedef int (*a320_read_packet_fn_t)(const struct device *dev, int8_t *dx, int8_t *dy);

struct a320_data {
    const struct device *dev;
    struct i2c_dt_spec i2c;
    a320_read_packet_fn_t read_packet;
    uint8_t detected_i2c_addr;
    struct k_work work;
    struct k_work_delayable motion_retry_work;
    struct gpio_callback motion_cb_data;
    struct k_work_delayable enable_irq_work;
    uint32_t last_packet_time;
    uint32_t last_scroll_time;
    uint32_t last_read_error_log;
    bool last_scroll_mode;
    bool last_smooth_scroll;
    float scroll_residue_x;
    float scroll_residue_y;
    int16_t motion_x_samples[MOTION_AVERAGE_SAMPLES];
    int16_t motion_y_samples[MOTION_AVERAGE_SAMPLES];
    uint8_t motion_sample_index;
    uint8_t motion_sample_count;
    uint32_t motion_sample_timer;
    int32_t arrow_accum_x;
    int32_t arrow_accum_y;
    uint32_t arrow_motion_timer;
    bool arrow_motion_active;
    atomic_val_t last_arrow_key_state;
};

struct a320_arrow_tap {
    uint16_t usage;
    atomic_val_t key_state;
};

K_MSGQ_DEFINE(a320_arrow_taps, sizeof(struct a320_arrow_tap), 8, 4);

static void a320_arrow_tap_work_cb(struct k_work *work);
K_WORK_DEFINE(a320_arrow_tap_work, a320_arrow_tap_work_cb);

/* Send a real keyboard arrow tap. Temporarily mask Shift in the outgoing
 * report so an arrow gesture never turns into text selection.
 */
static void tap_arrow_without_shift(uint16_t usage) {
    if (zmk_hid_keyboard_is_pressed(usage)) {
        return;
    }

    struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    zmk_mod_flags_t saved_mods = report->body.modifiers;
    int ret = zmk_hid_keyboard_press(usage);
    if (ret < 0) {
        return;
    }

    report->body.modifiers = saved_mods & ~(MOD_LSFT | MOD_RSFT);
    int press_err = zmk_endpoint_send_report(HID_USAGE_KEY);
    zmk_hid_keyboard_release(usage);
    int release_err = zmk_endpoint_send_report(HID_USAGE_KEY);
    report->body.modifiers = saved_mods;

    if (saved_mods & (MOD_LSFT | MOD_RSFT)) {
        zmk_endpoint_send_report(HID_USAGE_KEY);
    }
    if (press_err < 0 || release_err < 0) {
        LOG_DBG("Arrow report failed: press=%d release=%d", press_err, release_err);
    }
}

static void a320_arrow_tap_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    struct a320_arrow_tap tap;

    if (k_msgq_get(&a320_arrow_taps, &tap, K_NO_WAIT) == 0) {
        /* Bit 0 of the token is the effective arrow mode, so a tap queued in
         * one mode is dropped if the mode changed (or the mode key moved)
         * while it was waiting. */
        if ((tap.key_state & 1) && arrow_mode_token() == tap.key_state) {
            tap_arrow_without_shift(tap.usage);
            /* Direction-mode feedback: blank the trackpad LED for a moment (see
             * ARROW_BLANK_MS) so the mode is obvious without moving the finger
             * - see indicator_tp_arrow_fired. */
            indicator_tp_arrow_fired();
        }
    }

    if (k_msgq_num_used_get(&a320_arrow_taps) > 0) {
        k_work_submit(&a320_arrow_tap_work);
    }
}

static void reset_arrow_motion(struct a320_data *data) {
    data->arrow_accum_x = 0;
    data->arrow_accum_y = 0;
    data->arrow_motion_active = false;
}

/*
 * DPI the driver moves the pointer with.
 *
 * With the Studio trackpad module enabled the value lives in the module, so
 * the Studio slider, the &DPI keys and the driver all agree.  Without it the
 * driver keeps reading the SOFLE's own pointing-DPI value, which is what the
 * right half and the other shields using this driver still do.
 */
uint8_t a320_dpi_get(void) {
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    return (uint8_t)zitaotech_trackpad_dpi_value();
#else
    return sofle_touchpad_dpi_get();
#endif
}

static void process_arrow_swipe(struct a320_data *data, int32_t x, int32_t y, uint32_t now,
                                atomic_val_t key_state) {
    if (data->arrow_motion_active && now - data->arrow_motion_timer >= ARROW_GESTURE_RESET_MS) {
        reset_arrow_motion(data);
    }

    data->arrow_motion_timer = now;
    data->arrow_motion_active = true;
    data->arrow_accum_x += x;
    data->arrow_accum_y += y;

    int32_t abs_x = abs(data->arrow_accum_x);
    int32_t abs_y = abs(data->arrow_accum_y);
    int32_t swipe_threshold = trackpad_arrow_swipe_threshold();
    if (abs_x < swipe_threshold && abs_y < swipe_threshold) {
        return;
    }

    uint16_t usage;
    if (abs_x >= abs_y) {
        usage = data->arrow_accum_x > 0 ? HID_USAGE_KEY_KEYBOARD_RIGHTARROW
                                        : HID_USAGE_KEY_KEYBOARD_LEFTARROW;
    } else {
        usage = data->arrow_accum_y > 0 ? HID_USAGE_KEY_KEYBOARD_DOWNARROW
                                        : HID_USAGE_KEY_KEYBOARD_UPARROW;
    }

    reset_arrow_motion(data);
    struct a320_arrow_tap tap = {.usage = usage, .key_state = key_state};
    if (k_msgq_put(&a320_arrow_taps, &tap, K_NO_WAIT) == 0) {
        k_work_submit(&a320_arrow_tap_work);
    }
}

static void reset_motion_average(struct a320_data *data) {
    data->motion_sample_index = 0;
    data->motion_sample_count = 0;
}

/* The 0x37 controller benefits from the same three-sample average as Q20. */
static void apply_motion_average(struct a320_data *data, int16_t *x, int16_t *y, uint32_t now) {
    if (data->motion_sample_count > 0 &&
        now - data->motion_sample_timer >= MOTION_AVERAGE_RESET_MS) {
        reset_motion_average(data);
    }

    data->motion_x_samples[data->motion_sample_index] = *x;
    data->motion_y_samples[data->motion_sample_index] = *y;
    data->motion_sample_index = (data->motion_sample_index + 1) % MOTION_AVERAGE_SAMPLES;
    if (data->motion_sample_count < MOTION_AVERAGE_SAMPLES) {
        data->motion_sample_count++;
    }
    data->motion_sample_timer = now;

    int32_t sum_x = 0;
    int32_t sum_y = 0;
    for (uint8_t i = 0; i < data->motion_sample_count; i++) {
        sum_x += data->motion_x_samples[i];
        sum_y += data->motion_y_samples[i];
    }
    *x = sum_x / data->motion_sample_count;
    *y = sum_y / data->motion_sample_count;
}

/* Read the selected variant's registers, always releasing the I2C mutex. */
static int a320_read_registers(const struct device *dev, uint8_t reg, uint8_t *buf, size_t len) {
    struct a320_data *data = dev->data;
    k_mutex_lock(&a320_i2c_mutex, K_FOREVER);
    int ret = i2c_write_dt(&data->i2c, &reg, 1);
    if (ret < 0)
        goto out;

    /* The 0x37 protocol uses the reference controller's two address writes
     * followed by the burst read.
     */
    if (data->detected_i2c_addr == A320_I2C_ADDR_37) {
        ret = i2c_write_dt(&data->i2c, &reg, 1);
        if (ret < 0)
            goto out;
    }
    ret = i2c_burst_read_dt(&data->i2c, reg, buf, len);
out:
    k_mutex_unlock(&a320_i2c_mutex);
    return ret;
}

/* Keep the original 0x3B dx/dy orientation. */
static int a320_read_packet_3b(const struct device *dev, int8_t *dx, int8_t *dy) {
    uint8_t buf[3] = {0};
    int ret = a320_read_registers(dev, 0x82, buf, sizeof(buf));
    if (ret < 0)
        return ret;
    *dx = (int8_t)buf[1];
    *dy = -(int8_t)buf[2];
    return 0;
}

/* The 0x37 variant has a different packet layout. */
static int a320_read_packet_37(const struct device *dev, int8_t *dx, int8_t *dy) {
    uint8_t buf[7] = {0};
    int ret = a320_read_registers(dev, 0x0A, buf, sizeof(buf));
    if (ret < 0)
        return ret;
    *dy = -(int8_t)buf[1];
    *dx = -(int8_t)buf[3];
    return 0;
}

static bool a320_detect_variant_at_boot(const struct device *dev) {
    struct a320_data *data = dev->data;

    /* Default is always 0x37. Only a successful 0x3B boot probe changes the
     * selected address and packet format.
     */
    data->i2c.addr = A320_DEFAULT_I2C_ADDR;
    data->detected_i2c_addr = A320_DEFAULT_I2C_ADDR;
    data->read_packet = a320_read_packet_37;

    struct i2c_dt_spec probe = data->i2c;
    uint8_t reg = 0x82;
    probe.addr = A320_I2C_ADDR_3B;
    if (i2c_write_dt(&probe, &reg, 1) == 0) {
        data->i2c.addr = A320_I2C_ADDR_3B;
        data->detected_i2c_addr = A320_I2C_ADDR_3B;
        data->read_packet = a320_read_packet_3b;
        LOG_INF("Trackpad detected at I2C address 0x%02X", A320_I2C_ADDR_3B);
        return true;
    }

    LOG_INF("No 0x3B trackpad detected; using 0x37 protocol on MOTION");
    return false;
}

/* Same speed scaling and fractional accumulation as the Q20 CM5 driver. */
/* Provided by the ZitaoTech trackpad module (ZitaoTech Studio's switch). */
extern bool zitaotech_trackpad_smooth_scroll_enabled(void);

/* Pending wheel notches for smooth scrolling: the total distance stays the same
 * as in stepped mode, it is just handed to the host a notch at a time. */
/* Wheel input uses 1/16-notch units when the HID capability is compiled in.
 * The listener converts these to the resolution negotiated by the host. */
static inline void process_cm5_scroll(const struct device *dev, struct a320_data *data, int16_t dx,
                                      int16_t dy, uint32_t now) {
    ARG_UNUSED(now);
    bool smooth = false;
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL) && IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
    smooth = zitaotech_trackpad_smooth_scroll_enabled();
#endif
    if (smooth != data->last_smooth_scroll) {
        data->scroll_residue_x = data->scroll_residue_y = 0;
        data->last_smooth_scroll = smooth;
    }
    int abs_x = dx < 0 ? -dx : dx;
    int abs_y = dy < 0 ? -dy : dy;
    int abs_max = abs_x > abs_y ? abs_x : abs_y;
    if (abs_max <= 2) { return; }
    if (abs_max > 128) { abs_max = 128; }
    float t = (float)abs_max / 128.0f;
    /* Halve the base scroll rate, preserving the Studio percentage scale. */
    int divisor = 2 * (int)(24 - 18 * t * t);
    if (divisor < 1) { divisor = 1; }
    int precision = smooth ? 16 : 1;
    int32_t rx = (int32_t)data->scroll_residue_x + (int32_t)dx * precision;
    int32_t ry = (int32_t)data->scroll_residue_y + (int32_t)dy * precision;
    int16_t out_x = rx / divisor;
    int16_t out_y = ry / divisor;
    // Consume exactly what was emitted; do not queue or count it a second time.
    data->scroll_residue_x = ((rx - out_x * divisor) * 3) / 4;
    data->scroll_residue_y = ((ry - out_y * divisor) * 3) / 4;
#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
    if (!smooth) { out_x *= 16; out_y *= 16; }
#endif
    if (out_x || out_y) {
        input_report_rel(dev, INPUT_REL_HWHEEL, -out_x, false, K_FOREVER);
        input_report_rel(dev, INPUT_REL_WHEEL, -out_y, true, K_FOREVER);
    }
}
static void a320_process_pending_motion(struct a320_data *data);

static void a320_work_cb(struct k_work *work) {
    struct a320_data *data = CONTAINER_OF(work, struct a320_data, work);
    const struct device *dev = data->dev;
    uint32_t now = k_uptime_get_32();

    /* Bit 0 of the token is the effective arrow mode (the base mode inverted
     * while the mode key is held), so the rest of this function is unchanged. */
    atomic_val_t arrow_state = arrow_mode_token();
    bool arrow_mode = (arrow_state & 1) != 0;
    if (arrow_state != data->last_arrow_key_state) {
        reset_arrow_motion(data);
        reset_motion_average(data);
        data->scroll_residue_x = 0;
        data->scroll_residue_y = 0;
        data->last_scroll_mode = false;
        data->last_arrow_key_state = arrow_state;
    }

    /* ========= WATCHDOG ========= */
    if (now - last_activity_time > A320_WDT_TIMEOUT) {
        LOG_WRN("A320 watchdog recovery");
        data->scroll_residue_x = 0;
        data->scroll_residue_y = 0;
        data->last_scroll_mode = false;
        reset_motion_average(data);
        reset_arrow_motion(data);
        touched = false;
        /*
         * Do not just drop this work item: the sensor's MOTION output is
         * level, not edge, so a packet that is left unread keeps the line
         * asserted and no further interrupt arrives - the pad then looks dead
         * (and the LED stays off) until the line happens to drop again.  A
         * mode switch is enough to delay the work long enough to land here,
         * which is why it looked like switching modes killed the trackpad.
         * Re-check the line and read the packet if it is still there.
         */
        a320_process_pending_motion(data);
        return;
    }

    int8_t packet_dx = 0, packet_dy = 0;
    int16_t total_dx = 0;
    int16_t total_dy = 0;
    bool got_data = false;
    bool read_succeeded = false;

    while (1) {
        int ret = data->read_packet(dev, &packet_dx, &packet_dy);
        if (ret != 0) {
            if (now - data->last_read_error_log >= 1000) {
                LOG_WRN("Trackpad read failed at 0x%02X: %d", data->detected_i2c_addr, ret);
                data->last_read_error_log = now;
            }
            break;
        }

        read_succeeded = true;
        if (packet_dx == 0 && packet_dy == 0) {
            break;
        }
        total_dx += packet_dx;
        total_dy += packet_dy;
        got_data = true;

        /* 0x3B queues packets; 0x37 is sampled once per work invocation. */
        if (data->detected_i2c_addr == A320_I2C_ADDR_37) {
            break;
        }
    }

    /* MOTION may stay low after an empty packet or an I2C failure, so an
     * edge-only interrupt cannot guarantee another read. Continue 0x37
     * sampling (or retry a failed read) while active, yielding between reads.
     */
    if (data->detected_i2c_addr == A320_I2C_ADDR_37 || !read_succeeded) {
        const struct a320_config *cfg = dev->config;
        if (gpio_pin_get_dt(&cfg->motion_gpio) > 0) {
            k_work_reschedule(&data->motion_retry_work,
                              K_MSEC(read_succeeded ? A320_37_SAMPLE_MS : A320_READ_RETRY_MS));
        }
    }

    static uint32_t last_touch_time = 0;
    if (got_data) {
        last_touch_time = now;
        touched = true;
    }
    if (!got_data) {
        if (now - last_touch_time > TOUCH_IDLE_TIMEOUT) {
            touched = false;
        }
        return;
    }

    int16_t dx = total_dx;
    int16_t dy = total_dy;

    bool capslock = (zmk_hid_indicators_get_current_profile() & HID_INDICATORS_CAPS_LOCK) != 0;
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    /* Caps Lock scrolls on the stock firmware, but ZitaoTech Studio can turn
     * that behaviour off; the user-assignable scroll key always works. */
    capslock = capslock && zitaotech_trackpad_caps_lock_scroll_enabled();
#endif
    bool scroll_mode = !arrow_mode && (capslock || atomic_get(&scroll_key_pressed) != 0);

    /* Do not emit a report in the old mode if the mode changed during I2C. */
    if (arrow_mode_token() != arrow_state) {
        return;
    }

    if (scroll_mode && !data->last_scroll_mode) {
        data->scroll_residue_x = 0.0f;
        data->scroll_residue_y = 0.0f;
        data->last_scroll_time = 0;
    }

    if (arrow_mode) {
        reset_motion_average(data);
        /* This board's native X/Y orientation already matches screen axes. */

        /*
         * Direction mode does not move the pointer - the travel becomes arrow
         * keys inside the driver - so without this nothing would reach the
         * input subsystem at all, and the automatic mouse layer (which is
         * driven by the pad's input events) would never come up while the hand
         * is on the pad.  Report the touch itself with a zero-delta pair: the
         * host sees no movement, but the layer sees the pad.
         */
        input_report_rel(dev, INPUT_REL_X, 0, false, K_NO_WAIT);
        input_report_rel(dev, INPUT_REL_Y, 0, true, K_NO_WAIT);

        process_arrow_swipe(data, dx, dy, now, arrow_state);
    } else if (scroll_mode) {
        reset_motion_average(data);
        /* Keep this board's original X/Y axes and scroll directions, plus
         * whatever direction the user picked in ZitaoTech Studio. */
        int16_t scroll_x = dx * SCROLL_X_DIR * trackpad_scroll_x_sign();
        int16_t scroll_y = dy * SCROLL_Y_DIR * trackpad_scroll_y_sign();
        process_cm5_scroll(dev, data, scroll_x, scroll_y, now);
    } else if (!capslock) {
        if (data->detected_i2c_addr == A320_I2C_ADDR_37) {
            apply_motion_average(data, &dx, &dy, now);
        }

        float a320_factor = 0.4f + 0.01f * a320_dpi_get();

        /* Fast/slow mode scale the raw sample, then burst mode multiplies the
         * pointer speed on top of the DPI step. */
        bool fast_mode = atomic_get(&fast_key_pressed) != 0;
        if (!fast_mode && atomic_get(&slow_key_pressed)) {
            dx /= 2;
            dy /= 2;
        }

        float speed_mult = fast_mode ? 2.0f : 1.0f;
        float burst = trackpad_pointer_burst_scale();
        float fx = dx * 3 / 4 * a320_factor * speed_mult * burst;
        float fy = dy * 3 / 4 * a320_factor * speed_mult * burst;

        input_report_rel(dev, INPUT_REL_X, (int)fx, false, K_NO_WAIT);
        input_report_rel(dev, INPUT_REL_Y, (int)fy, true, K_NO_WAIT);
    } else {
        touched = false;
    }

    data->last_scroll_mode = scroll_mode;
    touched = false;
    data->last_packet_time = now;
}

static void a320_motion_retry_work_cb(struct k_work *work) {
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct a320_data *data = CONTAINER_OF(dwork, struct a320_data, motion_retry_work);
    const struct a320_config *cfg = data->dev->config;

    if (gpio_pin_get_dt(&cfg->motion_gpio) > 0) {
        last_activity_time = k_uptime_get_32();
        k_work_submit_to_queue(&a320_workq, &data->work);
    }
}

/* ========= GPIO ISR ========= */
static void motion_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
    struct a320_data *data = CONTAINER_OF(cb, struct a320_data, motion_cb_data);

    last_activity_time = k_uptime_get_32();
    indicator_tp_motion_triggered();

    /* ⭐ 防止 work 堆积 */
    k_work_submit_to_queue(&a320_workq, &data->work);
}

/* An edge interrupt will not fire when MOTION is already active at startup. */
static void a320_process_pending_motion(struct a320_data *data) {
    const struct a320_config *cfg = data->dev->config;
    int active = gpio_pin_get_dt(&cfg->motion_gpio);

    if (active < 0) {
        LOG_WRN("Failed to read A320 MOTION pin: %d", active);
        return;
    }

    if (active) {
        last_activity_time = k_uptime_get_32();
        indicator_tp_motion_triggered();
        k_work_submit_to_queue(&a320_workq, &data->work);
    }
}

bool tp_is_touched(void) { return touched; }

static void a320_enable_irq_work_cb(struct k_work *work) {
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct a320_data *data = CONTAINER_OF(dwork, struct a320_data, enable_irq_work);
    const struct device *dev = data->dev;
    const struct a320_config *cfg = dev->config;

    gpio_pin_interrupt_configure_dt(&cfg->motion_gpio, GPIO_INT_EDGE_TO_ACTIVE);
    a320_process_pending_motion(data);

    LOG_INF("A320 IRQ enabled (delayed)");
}
/* ========= 初始化 ========= */
static int a320_init(const struct device *dev) {
    const struct a320_config *cfg = dev->config;
    struct a320_data *data = dev->data;

    if (!i2c_is_ready_dt(&cfg->i2c))
        return -ENODEV;
    if (!gpio_is_ready_dt(&cfg->motion_gpio))
        return -ENODEV;

    k_mutex_init(&a320_i2c_mutex);

    data->dev = dev;
    data->i2c = cfg->i2c;

    int ret = gpio_pin_configure_dt(&cfg->motion_gpio, GPIO_INPUT);
    if (ret < 0)
        return ret;

    /* Allow both controller variants to finish power-on before probing. */
    k_msleep(10);
    a320_detect_variant_at_boot(dev);

    k_work_init(&data->work, a320_work_cb);
    k_work_init_delayable(&data->motion_retry_work, a320_motion_retry_work_cb);

    /* ⭐ 启动 workqueue */
    k_work_queue_start(&a320_workq, a320_workq_stack, K_THREAD_STACK_SIZEOF(a320_workq_stack),
                       A320_WORKQ_PRIORITY, NULL);

    gpio_init_callback(&data->motion_cb_data, motion_isr, BIT(cfg->motion_gpio.pin));
    ret = gpio_add_callback(cfg->motion_gpio.port, &data->motion_cb_data);
    if (ret < 0)
        return ret;

    ret = gpio_pin_interrupt_configure_dt(&cfg->motion_gpio, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0)
        return ret;
    a320_process_pending_motion(data);

    k_work_init_delayable(&data->enable_irq_work, a320_enable_irq_work_cb);
    k_work_schedule(&data->enable_irq_work, K_MSEC(200));

    LOG_INF("A320 Driver Initialized (addr=0x%02X, I2C mutex enabled)", data->detected_i2c_addr);
    return 0;
}

#define A320_DEFINE(inst)                                                                          \
    static struct a320_data a320_data_##inst;                                                      \
    static const struct a320_config a320_config_##inst = {                                         \
        .i2c = I2C_DT_SPEC_INST_GET(inst),                                                         \
        .motion_gpio = {.port = DEVICE_DT_GET(MOTION_GPIO_NODE),                                   \
                        .pin = MOTION_GPIO_PIN,                                                    \
                        .dt_flags = MOTION_GPIO_FLAGS},                                            \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, a320_init, NULL, &a320_data_##inst, &a320_config_##inst,           \
                          POST_KERNEL, 70, NULL);

DT_INST_FOREACH_STATUS_OKAY(A320_DEFINE);
