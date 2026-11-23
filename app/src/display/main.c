/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/atomic.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/drivers/display.h>
#include <zephyr/drivers/led.h>
#include <lvgl.h>

#include "theme.h"

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/display/status_screen.h>
#include <zmk/bootloader_feedback.h>

static const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

#if DT_HAS_CHOSEN(zmk_display_led)

static const struct device *display_led = DEVICE_DT_GET(DT_PARENT(DT_CHOSEN(zmk_display_led)));
static const uint8_t display_led_idx = DT_NODE_CHILD_IDX(DT_CHOSEN(zmk_display_led));

#endif

static bool initialized = false;
static atomic_t bootloader_pending;
extern void zmk_memory_lcd_hold_frame(const struct device *dev) __attribute__((weak));

static lv_obj_t *screen;

__attribute__((weak)) lv_obj_t *zmk_display_status_screen() { return NULL; }

void display_tick_cb(struct k_work *work) {
    if (!atomic_get(&bootloader_pending)) lv_task_handler();
}

K_WORK_DEFINE(display_tick_work, display_tick_cb);

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_WORK_QUEUE_DEDICATED)

K_THREAD_STACK_DEFINE(display_work_stack_area, CONFIG_ZMK_DISPLAY_DEDICATED_THREAD_STACK_SIZE);

static struct k_work_q display_work_q;

#endif

struct k_work_q *zmk_display_work_q() {
#if IS_ENABLED(CONFIG_ZMK_DISPLAY_WORK_QUEUE_DEDICATED)
    return &display_work_q;
#else
    return &k_sys_work_q;
#endif
}

void display_timer_cb() {
    if (!atomic_get(&bootloader_pending))
        k_work_submit_to_queue(zmk_display_work_q(), &display_tick_work);
}

K_TIMER_DEFINE(display_timer, display_timer_cb, NULL);

/* Draw directly on the panel: Bootmagic can run before LVGL initializes.
 * Normal transitions run on the display queue to serialize existing renders. */
static const uint8_t bootloader_glyphs[][5] = {
    {0x7f,0x49,0x49,0x49,0x36}, /* B */
    {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x04,0x3f,0x44,0x40,0x20}, /* t */
    {0x00,0x41,0x7f,0x40,0x00}, /* l */
    {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x38,0x44,0x44,0x48,0x7f}, /* d */
    {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x7c,0x08,0x04,0x04,0x08}, /* r */
    {0,0,0,0,0},               /* space */
    {0x7c,0x04,0x18,0x04,0x78}, /* m */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
};
static const uint8_t bootloader_text[] = {0,1,1,2,3,1,4,5,6,7,8,9,1,5,6};
static const uint8_t bootloader_portrait_text[] = {0,1,1,2,10,3,1,4,5,6,7,9,1,5,6};

static bool bootloader_pixel(int x, int y, int width, int height, bool portrait) {
    int scale = 2;
    const uint8_t *text = portrait ? bootloader_portrait_text : bootloader_text;
    int first = 0, count = sizeof(bootloader_text);
    int top = (height - 7 * scale) / 2;
    if (portrait) {
        /* Three centered lines, with 2x glyphs: Boot- / loader / mode. */
        top = (height - 62) / 2;
        if (y >= top + 48) { first = 11; count = 4; top += 48; }
        else if (y >= top + 24) { first = 5; count = 6; top += 24; }
        else count = 5;
    }
    int left = (width - (count * 6 - 1) * scale) / 2;
    x -= left; y -= top;
    if (x < 0 || y < 0 || y >= 7 * scale || x >= count * 6 * scale) return false;
    int col = (x / scale) % 6;
    return col < 5 && (bootloader_glyphs[text[first + x / (6 * scale)]][col] & BIT(y / scale));
}

static void bootloader_draw(void) {
    if (!device_is_ready(display)) return;
    struct display_capabilities caps;
    display_get_capabilities(display, &caps);
    if (caps.x_resolution > 240 || caps.y_resolution > 240 ||
        !(caps.supported_pixel_formats & PIXEL_FORMAT_RGB_565)) return;
    if (caps.current_pixel_format != PIXEL_FORMAT_RGB_565 &&
        display_set_pixel_format(display, PIXEL_FORMAT_RGB_565) < 0) return;
    bool portrait = caps.x_resolution == 144 && caps.y_resolution == 72;
    uint16_t row[240];
    struct display_buffer_descriptor desc = {
        .buf_size = caps.x_resolution * sizeof(uint16_t),
        .width = caps.x_resolution, .height = 1, .pitch = caps.x_resolution,
    };
    if (portrait) {
        /* One native column is one panel gate line: avoid rewriting every
         * gate line once per pixel row on the rotated memory LCD. */
        desc.width = 1; desc.height = 72; desc.pitch = 1;
        desc.buf_size = 72 * sizeof(uint16_t);
        for (int x = 0; x < 144; x++) {
            for (int y = 0; y < 72; y++)
                row[y] = bootloader_pixel(71 - y, x, 72, 144, true) ? 0 : 0x07ff;
            if (display_write(display, x, 0, &desc, row) < 0) break;
        }
    } else {
    for (int y = 0; y < caps.y_resolution; y++) {
        for (int x = 0; x < caps.x_resolution; x++) {
            bool lit = portrait ? bootloader_pixel(71 - y, x, 72, 144, true)
                                : bootloader_pixel(x, y, caps.x_resolution, caps.y_resolution, false);
            row[x] = lit ? 0xffff : 0;
        }
        if (display_write(display, 0, y, &desc, row) < 0) break;
    }
    }
    display_blanking_off(display);
#if DT_HAS_CHOSEN(zmk_display_backlight)
    const struct device *backlight = DEVICE_DT_GET(DT_CHOSEN(zmk_display_backlight));
    if (device_is_ready(backlight)) led_set_brightness(backlight, 0, 100);
#endif
#if DT_HAS_CHOSEN(zmk_display_led)
    led_on(display_led, display_led_idx);
#endif
}

static int bootloader_reboot_type;
static void bootloader_transition(struct k_work *work) {
    k_timer_stop(&display_timer);
    bootloader_draw();
    /* A powered memory LCD retains its final frame. Block queued rendering and
     * PM reinitialization before reboot instead of clearing/blanking the panel. */
    if (zmk_memory_lcd_hold_frame) zmk_memory_lcd_hold_frame(display);
    /* Give the panel a visible transition before the bootloader takes over. */
    k_sleep(K_MSEC(350));
    sys_reboot(bootloader_reboot_type);
}
K_WORK_DEFINE(bootloader_transition_work, bootloader_transition);

void zmk_bootloader_feedback_reboot(int type) {
    if (!atomic_cas(&bootloader_pending, 0, 1)) return;
    bootloader_reboot_type = type;
    k_timer_stop(&display_timer);
    if (initialized) {
        if (k_work_submit_to_queue(zmk_display_work_q(), &bootloader_transition_work) >= 0) return;
    }
    bootloader_transition(NULL);
}

void unblank_display_cb(struct k_work *work) {
    if (atomic_get(&bootloader_pending)) return;
#if DT_HAS_CHOSEN(zmk_display_led)
    led_on(display_led, display_led_idx);
#endif
    display_blanking_off(display);
#if !IS_ENABLED(CONFIG_ARCH_POSIX)
    k_timer_start(&display_timer, K_MSEC(CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS),
                  K_MSEC(CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS));
#endif // !IS_ENABLED(CONFIG_ARCH_POSIX)
}

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE)

void blank_display_cb(struct k_work *work) {
    if (atomic_get(&bootloader_pending)) return;
#if !IS_ENABLED(CONFIG_ARCH_POSIX)
    k_timer_stop(&display_timer);
#endif // !IS_ENABLED(CONFIG_ARCH_POSIX)
    display_blanking_on(display);
#if DT_HAS_CHOSEN(zmk_display_led)
    led_off(display_led, display_led_idx);
#endif
}
K_WORK_DEFINE(blank_display_work, blank_display_cb);
K_WORK_DEFINE(unblank_display_work, unblank_display_cb);

static void start_display_updates() {
    if (display == NULL) {
        return;
    }

    k_work_submit_to_queue(zmk_display_work_q(), &unblank_display_work);
}

static void stop_display_updates() {
    if (display == NULL) {
        return;
    }

    k_work_submit_to_queue(zmk_display_work_q(), &blank_display_work);
}

#endif

int zmk_display_is_initialized() { return initialized; }

static void initialize_theme() {
#if IS_ENABLED(CONFIG_LV_USE_THEME_MONO)
    lv_disp_t *disp = lv_disp_get_default();
    lv_theme_t *theme =
        lv_theme_mono_init(disp, IS_ENABLED(CONFIG_ZMK_DISPLAY_INVERT), CONFIG_LV_FONT_DEFAULT,
                           CONFIG_ZMK_LV_FONT_DEFAULT_SMALL, CONFIG_LV_FONT_DEFAULT);

    lv_display_set_theme(disp, theme);
#endif // CONFIG_LV_USE_THEME_MONO
}

void initialize_display(struct k_work *work) {
    if (atomic_get(&bootloader_pending)) return;
    LOG_DBG("");

    if (!device_is_ready(display)) {
        LOG_ERR("Failed to find display device");
        return;
    }

    initialized = true;

    initialize_theme();

    screen = zmk_display_status_screen();

    if (screen == NULL) {
        LOG_ERR("No status screen provided");
        return;
    }

    lv_scr_load(screen);

    unblank_display_cb(work);
}

K_WORK_DEFINE(init_work, initialize_display);

int zmk_display_init() {
#if IS_ENABLED(CONFIG_ZMK_DISPLAY_WORK_QUEUE_DEDICATED)
    struct k_work_queue_config config = {
#if IS_ENABLED(CONFIG_THREAD_NAME)
        .name = "display queue",
#endif
    };
    k_work_queue_start(&display_work_q, display_work_stack_area,
                       K_THREAD_STACK_SIZEOF(display_work_stack_area),
                       CONFIG_ZMK_DISPLAY_DEDICATED_THREAD_PRIORITY, &config);
#endif

#if IS_ENABLED(CONFIG_ARCH_POSIX)
    initialize_display(NULL);
#else
    k_work_submit_to_queue(zmk_display_work_q(), &init_work);
#endif

    LOG_DBG("");
    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE)
int display_event_handler(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return -ENOTSUP;
    }

    switch (ev->state) {
    case ZMK_ACTIVITY_ACTIVE:
        start_display_updates();
        break;
    case ZMK_ACTIVITY_IDLE:
    case ZMK_ACTIVITY_SLEEP:
        stop_display_updates();
        break;
    default:
        LOG_WRN("Unhandled activity state: %d", ev->state);
        return -EINVAL;
    }
    return 0;
}

ZMK_LISTENER(display, display_event_handler);
ZMK_SUBSCRIPTION(display, zmk_activity_state_changed);

#endif /* IS_ENABLED(CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE) */
