/*
 * A320 optical sensor driver (0x57 / 0x3B, pointer speed and arrow gestures)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT avago_a320

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>

#include <math.h>
#include <stdlib.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <dt-bindings/zmk/modifiers.h>

#include "trackpad_led.h"
#include "a320.h"

LOG_MODULE_REGISTER(a320, CONFIG_A320_LOG_LEVEL);

/* =========================
 * Config
 * ========================= */

#define A320_I2C_ADDR_57 0x57
#define A320_I2C_ADDR_3B 0x3B
#define A320_DEFAULT_I2C_ADDR A320_I2C_ADDR_57
#define MOTION_AVERAGE_SAMPLES 3
#define MOTION_AVERAGE_RESET_MS 50
#define TRACKPAD_SETTLE_SAMPLES 4
#define TRACKPAD_NEW_TOUCH_GAP_MS 200
#define FAST_KEY_POSITION 37
#define SLOW_KEY_POSITION 36
#define ARROW_KEY_POSITION 27
#define FAST_KEY_MULTIPLIER 2.0f
#define SLOW_KEY_DIVIDER 2
#define ARROW_SWIPE_THRESHOLD 34
#define ARROW_GESTURE_RESET_MS 120

#define A320_DPI_MIN 10
#define A320_DPI_MAX 100
#define A320_DPI_STEP 10
/* Legacy BBP9981 used backlight brightness 40: 0.4 + 0.01 * 40 = 0.8. */
#define A320_DPI_DEFAULT 40
static atomic_t trackpad_dpi = ATOMIC_INIT(A320_DPI_DEFAULT);

#define A320_57_SAMPLE_MS 5
#define A320_3B_SAMPLE_MS 2
#define A320_READ_RETRY_MS 20
#define A320_WORKQ_STACK_SIZE 2048
#define A320_WORKQ_PRIORITY 5

K_THREAD_STACK_DEFINE(a320_workq_stack, A320_WORKQ_STACK_SIZE);
static struct k_work_q a320_workq;

/* =========================
 * HID indicators
 * ========================= */
static atomic_t current_indicators;

#define HID_INDICATORS_CAPS_LOCK (1 << 1)

/* =========================
 * Motion GPIO
 * ========================= */

#define MOTION_GPIO_NODE DT_NODELABEL(gpio0)
#define MOTION_GPIO_PIN 2
static const struct device *motion_gpio_dev;

/* =========================
 * State flags
 * ========================= */

static atomic_t touched;
static atomic_t slow_key_pressed = ATOMIC_INIT(0);
static atomic_t fast_key_pressed = ATOMIC_INIT(0);
/* Odd = held, even = released; each edge also invalidates queued gestures. */
static atomic_t arrow_key_state = ATOMIC_INIT(0);

/* =========================
 * Data & Config structs
 * ========================= */

struct a320_dev_config {
    struct i2c_dt_spec i2c;
};

typedef int (*a320_read_fn_t)(const struct device *dev, int16_t *dx, int16_t *dy);

struct a320_data {
    const struct device *dev;
    struct i2c_dt_spec i2c;
    uint8_t detected_i2c_addr;
    struct k_work motion_work;
    struct k_work_delayable motion_retry_work;
    struct k_work_delayable startup_work;
    struct gpio_callback motion_cb;
    a320_read_fn_t read_motion;
    int16_t motion_x_samples[MOTION_AVERAGE_SAMPLES];
    int16_t motion_y_samples[MOTION_AVERAGE_SAMPLES];
    uint8_t motion_sample_index;
    uint8_t motion_sample_count;
    uint32_t motion_sample_timer;
    bool touch_sample_started;
    uint8_t touch_settle_count;
    uint32_t touch_sample_timer;
    int32_t arrow_accum_x;
    int32_t arrow_accum_y;
    uint32_t arrow_motion_timer;
    bool arrow_motion_active;
    atomic_val_t last_arrow_key_state;
    bool last_scroll_mode;
    float scroll_residue_x;
    float scroll_residue_y;
    uint32_t last_scroll_time;
};

struct a320_arrow_tap {
    uint16_t usage;
    atomic_val_t key_state;
};

K_MSGQ_DEFINE(a320_arrow_taps, sizeof(struct a320_arrow_tap), 8, 4);

static void a320_arrow_tap_work_cb(struct k_work *work);
K_WORK_DEFINE(a320_arrow_tap_work, a320_arrow_tap_work_cb);

/* Defer each tap to the system queue alongside normal key handling.
 * Mask only the outgoing report; preserve ZMK's modifier counts and masks.
 */
static void tap_arrow_without_shift(uint16_t usage) {
    if (zmk_hid_keyboard_is_pressed(usage)) {
        return; /* Do not release an arrow that the user is already holding. */
    }

    struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    zmk_mod_flags_t saved_mods = report->body.modifiers;
    int ret = zmk_hid_keyboard_press(usage);
    if (ret < 0) {
        return;
    }
    report->body.modifiers = saved_mods & ~(MOD_LSFT | MOD_RSFT);
    int press_err = zmk_endpoints_send_report(HID_USAGE_KEY);
    zmk_hid_keyboard_release(usage);
    int release_err = zmk_endpoints_send_report(HID_USAGE_KEY);
    report->body.modifiers = saved_mods;
    if (saved_mods & (MOD_LSFT | MOD_RSFT)) {
        zmk_endpoints_send_report(HID_USAGE_KEY);
    }
    if (press_err < 0 || release_err < 0) {
        LOG_DBG("Arrow report failed: press=%d release=%d", press_err, release_err);
    }
}

static void a320_arrow_tap_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    struct a320_arrow_tap tap;
    if (k_msgq_get(&a320_arrow_taps, &tap, K_NO_WAIT) == 0) {
        if ((tap.key_state & 1) && atomic_get(&arrow_key_state) == tap.key_state) {
            tap_arrow_without_shift(tap.usage);
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
    if (abs_x < ARROW_SWIPE_THRESHOLD && abs_y < ARROW_SWIPE_THRESHOLD) {
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

/* Only the 0x57 pointer path uses this three-sample moving average. */
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

/* MOTION can pulse per packet, so use the reference driver's sample-gap
 * heuristic instead of resetting on every GPIO edge. Call only after a
 * successful read, including zero-motion packets.
 */
static bool trackpad_motion_is_stable(struct a320_data *data, uint32_t now) {
    if (!data->touch_sample_started ||
        now - data->touch_sample_timer >= TRACKPAD_NEW_TOUCH_GAP_MS) {
        data->touch_sample_started = true;
        data->touch_settle_count = 0;
        reset_motion_average(data);
        reset_arrow_motion(data);
        data->scroll_residue_x = 0;
        data->scroll_residue_y = 0;
        data->last_scroll_time = 0;
    }
    data->touch_sample_timer = now;

    if (data->touch_settle_count < TRACKPAD_SETTLE_SAMPLES) {
        data->touch_settle_count++;
        return false;
    }
    return true;
}

/* =========================
 * Key listener
 * ========================= */

static int key_listener_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (!ev)
        return 0;

    if (ev->position == FAST_KEY_POSITION) {
        atomic_set(&fast_key_pressed, ev->state);
    } else if (ev->position == SLOW_KEY_POSITION) {
        atomic_set(&slow_key_pressed, ev->state);
    } else if (ev->position == ARROW_KEY_POSITION) {
        if (((atomic_get(&arrow_key_state) & 1) != 0) != ev->state) {
            atomic_inc(&arrow_key_state);
        }
    }

    return 0;
}

ZMK_LISTENER(a320_key_listener, key_listener_cb);
ZMK_SUBSCRIPTION(a320_key_listener, zmk_position_state_changed);

/* =========================
 * HID indicator listener
 * ========================= */

static int hid_indicators_listener(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);

    if (ev)
        atomic_set(&current_indicators, ev->indicators);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(a320_hid_listener, hid_indicators_listener);
ZMK_SUBSCRIPTION(a320_hid_listener, zmk_hid_indicators_changed);

/* =========================
 * I2C read
 * ========================= */

static int a320_read_motion_3b(const struct device *dev, int16_t *dx, int16_t *dy) {
    struct a320_data *data = dev->data;
    uint8_t buf[3];
    uint8_t reg = 0x82;

    if (i2c_write_dt(&data->i2c, &reg, 1) < 0)
        return -EIO;

    if (i2c_burst_read_dt(&data->i2c, 0x82, buf, sizeof(buf)) < 0)
        return -EIO;

    *dx = (int8_t)buf[1];
    *dy = -(int8_t)buf[2];

    return 0;
}

static int a320_read_motion_57(const struct device *dev, int16_t *dx, int16_t *dy) {
    struct a320_data *data = dev->data;
    uint8_t buf[7];
    uint8_t reg = 0x0A;

    if (i2c_write_dt(&data->i2c, &reg, 1) < 0)
        return -EIO;

    if (i2c_burst_read_dt(&data->i2c, 0x0A, buf, sizeof(buf)) < 0)
        return -EIO;

    *dx = (int8_t)buf[3];
    *dy = (int8_t)buf[1];

    return 0;
}

/* Select the address/protocol once at boot, with a usable 0x57 fallback. */
static bool a320_detect_variant_at_boot(const struct device *dev) {
    struct a320_data *data = dev->data;
    const uint8_t candidates[] = {A320_I2C_ADDR_57, A320_I2C_ADDR_3B};
    const uint8_t probe_registers[] = {0x0A, 0x82};

    data->i2c.addr = A320_DEFAULT_I2C_ADDR;
    data->detected_i2c_addr = A320_DEFAULT_I2C_ADDR;
    data->read_motion = a320_read_motion_57;
    struct i2c_dt_spec probe = data->i2c;

    for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
        probe.addr = candidates[i];

        /* A register-address write is the probe used by both sensor models.
         * A raw I2C read can NACK even when the trackpad is present.
         */
        if (i2c_write_dt(&probe, &probe_registers[i], 1) == 0) {
            data->i2c.addr = candidates[i];
            data->detected_i2c_addr = candidates[i];
            data->read_motion =
                (candidates[i] == A320_I2C_ADDR_3B) ? a320_read_motion_3b : a320_read_motion_57;
            LOG_INF("Trackpad detected at I2C address 0x%02X", candidates[i]);
            return true;
        }
    }

    /* Keep the original raw-read probe as a compatibility fallback. */
    const uint8_t read_candidates[] = {A320_I2C_ADDR_3B, A320_I2C_ADDR_57};
    for (size_t i = 0; i < ARRAY_SIZE(read_candidates); i++) {
        uint8_t test_byte;
        probe.addr = read_candidates[i];
        if (i2c_read_dt(&probe, &test_byte, 1) == 0) {
            data->i2c.addr = read_candidates[i];
            data->detected_i2c_addr = read_candidates[i];
            data->read_motion = (read_candidates[i] == A320_I2C_ADDR_3B) ? a320_read_motion_3b
                                                                         : a320_read_motion_57;
            LOG_INF("Trackpad detected at I2C address 0x%02X", read_candidates[i]);
            return true;
        }
    }

    /* No response at boot: keep 0x57 and read it directly on MOTION.
     * Runtime read failures must not trigger address detection again.
     */
    return false;
}

static inline void process_cm5_scroll(const struct device *dev, struct a320_data *data, int16_t dx,
                                      int16_t dy, uint32_t now) {
    if (now - data->last_scroll_time > 60) {
        data->scroll_residue_x = 0.0f;
        data->scroll_residue_y = 0.0f;
    }

    data->last_scroll_time = now;

    float speed = sqrtf((float)dx * dx + (float)dy * dy);
    float scale;

    if (speed > 80.0f)
        scale = 0.05f;
    else if (speed > 40.0f)
        scale = 0.04f;
    else if (speed > 20.0f)
        scale = 0.03f;
    else if (speed > 5.0f)
        scale = 0.02f;
    else
        scale = 0.015f;

    data->scroll_residue_x += dx * scale;
    data->scroll_residue_y += dy * scale;

    int16_t out_x = (int16_t)data->scroll_residue_x;
    int16_t out_y = (int16_t)data->scroll_residue_y;

    data->scroll_residue_x -= out_x;
    data->scroll_residue_y -= out_y;

    if (out_x || out_y) {
        input_report_rel(dev, INPUT_REL_HWHEEL, out_x, false, K_FOREVER);
        input_report_rel(dev, INPUT_REL_WHEEL, -out_y, true, K_FOREVER);
    }
}

/* =========================
 * Interrupt-driven motion work
 * ========================= */

static void a320_motion_work_handler(struct k_work *work) {
    struct a320_data *data = CONTAINER_OF(work, struct a320_data, motion_work);
    const struct device *dev = data->dev;
    uint32_t now = k_uptime_get_32();
    atomic_val_t arrow_state = atomic_get(&arrow_key_state);
    bool arrow_mode = (arrow_state & 1) != 0;
    bool scroll_mode =
        !arrow_mode && (atomic_get(&current_indicators) & HID_INDICATORS_CAPS_LOCK) != 0;

    if (arrow_state != data->last_arrow_key_state || scroll_mode != data->last_scroll_mode) {
        reset_arrow_motion(data);
        reset_motion_average(data);
        data->scroll_residue_x = 0;
        data->scroll_residue_y = 0;
        data->last_scroll_time = 0;
        data->last_arrow_key_state = arrow_state;
        data->last_scroll_mode = scroll_mode;
    }

    /* Read even if a short MOTION pulse ended before this work ran. */
    int read_ret;
    {
        int16_t dx = 0, dy = 0;
        read_ret = data->read_motion(dev, &dx, &dy);
        if (read_ret == 0 && !trackpad_motion_is_stable(data, now)) {
            goto reschedule;
        }
        if (read_ret == 0 && (dx || dy)) {
            if (atomic_get(&arrow_key_state) != arrow_state) {
                goto reschedule;
            }

            if (arrow_mode) {
                reset_motion_average(data);
                /* BBP9981 reports dx/dy directly, without Q20's axis rotation. */
                process_arrow_swipe(data, dx, dy, now, arrow_state);
            } else if (scroll_mode) {
                reset_motion_average(data);
                process_cm5_scroll(dev, data, dx, dy, now);
            } else {
                if (data->i2c.addr == A320_I2C_ADDR_57) {
                    apply_motion_average(data, &dx, &dy, now);
                }
                float tp_factor = 0.4f + 0.01f * a320_dpi_get();
                bool fast_mode = atomic_get(&fast_key_pressed) != 0;
                /* Match the legacy slow-key truncation before base scaling.
                 * Fast still takes priority when both speed keys are held.
                 */
                if (!fast_mode && atomic_get(&slow_key_pressed)) {
                    dx /= SLOW_KEY_DIVIDER;
                    dy /= SLOW_KEY_DIVIDER;
                }
                float speed_mult = fast_mode ? FAST_KEY_MULTIPLIER : 1.0f;
                dx = dx * 3 / 2 * tp_factor * speed_mult;
                dy = dy * 3 / 2 * tp_factor * speed_mult;
                input_report_rel(dev, INPUT_REL_X, dx, false, K_FOREVER);
                input_report_rel(dev, INPUT_REL_Y, dy, true, K_FOREVER);
                atomic_set(&touched, 1);
            }
        }
    }

reschedule:
    /* Continue only while MOTION stays low; no periodic wakeups while idle.
     * An empty packet or failed I2C read must not wait for another edge.
     */
    if (gpio_pin_get(motion_gpio_dev, MOTION_GPIO_PIN) == 0) {
        uint32_t delay_ms = read_ret < 0                         ? A320_READ_RETRY_MS
                            : data->i2c.addr == A320_I2C_ADDR_57 ? A320_57_SAMPLE_MS
                                                                 : A320_3B_SAMPLE_MS;
        k_work_reschedule(&data->motion_retry_work, K_MSEC(delay_ms));
    } else {
        atomic_set(&touched, 0);
    }
}

static void a320_motion_retry_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct a320_data *data = CONTAINER_OF(dwork, struct a320_data, motion_retry_work);
    if (gpio_pin_get(motion_gpio_dev, MOTION_GPIO_PIN) == 0) {
        k_work_submit_to_queue(&a320_workq, &data->motion_work);
    }
}

static void a320_motion_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
    struct a320_data *data = CONTAINER_OF(cb, struct a320_data, motion_cb);
    int level = gpio_pin_get(motion_gpio_dev, MOTION_GPIO_PIN);
    if (level < 0) {
        return;
    }
    bool active = level == 0;
    if (active) {
        indicator_tp_motion_triggered();
        k_work_submit_to_queue(&a320_workq, &data->motion_work);
    } else {
        atomic_set(&touched, 0);
    }
}

static void a320_process_pending_motion(struct a320_data *data) {
    if (gpio_pin_get(motion_gpio_dev, MOTION_GPIO_PIN) == 0) {
        indicator_tp_motion_triggered();
        k_work_submit_to_queue(&a320_workq, &data->motion_work);
    }
}

static void a320_startup_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct a320_data *data = CONTAINER_OF(dwork, struct a320_data, startup_work);
    a320_process_pending_motion(data);
}

bool tp_is_touched(void) { return atomic_get(&touched) != 0; }

uint8_t a320_dpi_get(void) { return (uint8_t)atomic_get(&trackpad_dpi); }

void a320_dpi_increase(void) {
    atomic_val_t dpi = atomic_get(&trackpad_dpi);
    dpi = MIN(dpi + A320_DPI_STEP, A320_DPI_MAX);
    atomic_set(&trackpad_dpi, dpi);
    indicator_tp_dpi_changed();
    LOG_INF("Trackpad DPI increased to %d", (int)dpi);
}

void a320_dpi_decrease(void) {
    atomic_val_t dpi = atomic_get(&trackpad_dpi);
    dpi = MAX(dpi - A320_DPI_STEP, A320_DPI_MIN);
    atomic_set(&trackpad_dpi, dpi);
    indicator_tp_dpi_changed();
    LOG_INF("Trackpad DPI decreased to %d", (int)dpi);
}

/* =========================
 * Init
 * ========================= */
static int a320_init(const struct device *dev) {
    const struct a320_dev_config *cfg = dev->config;
    struct a320_data *data = dev->data;

    if (!device_is_ready(cfg->i2c.bus))
        return -ENODEV;

    motion_gpio_dev = DEVICE_DT_GET(MOTION_GPIO_NODE);
    if (!device_is_ready(motion_gpio_dev))
        return -ENODEV;

    int ret = gpio_pin_configure(motion_gpio_dev, MOTION_GPIO_PIN, GPIO_INPUT | GPIO_PULL_UP);
    if (ret < 0)
        return ret;

    data->dev = dev;
    data->i2c = cfg->i2c;
    k_msleep(10);
    if (!a320_detect_variant_at_boot(dev)) {
        LOG_WRN("Trackpad not detected at boot; using 0x57 protocol on MOTION");
    }
    LOG_INF("A320 selected I2C address 0x%02X", data->i2c.addr);

    k_work_init(&data->motion_work, a320_motion_work_handler);
    k_work_init_delayable(&data->motion_retry_work, a320_motion_retry_work_handler);
    k_work_init_delayable(&data->startup_work, a320_startup_work_handler);
    gpio_init_callback(&data->motion_cb, a320_motion_isr, BIT(MOTION_GPIO_PIN));
    ret = gpio_add_callback(motion_gpio_dev, &data->motion_cb);
    if (ret < 0)
        return ret;

    k_work_queue_start(&a320_workq, a320_workq_stack, K_THREAD_STACK_SIZEOF(a320_workq_stack),
                       A320_WORKQ_PRIORITY, NULL);
    /* Falling edges wake the LED and I2C work; rising edges clear touch state. */
    ret = gpio_pin_interrupt_configure(motion_gpio_dev, MOTION_GPIO_PIN, GPIO_INT_EDGE_BOTH);
    if (ret < 0) {
        gpio_remove_callback(motion_gpio_dev, &data->motion_cb);
        return ret;
    }
    a320_process_pending_motion(data);
    k_work_schedule(&data->startup_work, K_MSEC(200));
    LOG_INF("A320 init OK");
    return 0;
}

/* =========================
 * Device define
 * ========================= */

#define A320_INIT_PRIORITY CONFIG_INPUT_A320_INIT_PRIORITY

#define A320_DEFINE(inst)                                                                          \
    static struct a320_data a320_data_##inst;                                                      \
    static const struct a320_dev_config a320_cfg_##inst = {                                        \
        .i2c = I2C_DT_SPEC_INST_GET(inst),                                                         \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, a320_init, NULL, &a320_data_##inst, &a320_cfg_##inst, POST_KERNEL, \
                          A320_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(A320_DEFINE)
