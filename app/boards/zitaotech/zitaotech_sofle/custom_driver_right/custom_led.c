/*
 * custom_led_trackpoint_dpi.c
 * Show boot brightness and explicit TrackPoint DPI adjustments.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "custom_led.h"
#include "pointing_dpi.h"
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPOINT_CONTROL)
#include <zitaotech/zmk/trackpoint.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_custom_led),
             "Custom LED enabled but no zmk,custom_led chosen node found");

static const struct device *const led_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_custom_led));

#define CHILD_COUNT(...) +1
#define DT_NUM_CHILD(node_id) (DT_FOREACH_CHILD(node_id, CHILD_COUNT))
#define LED_NUM (DT_NUM_CHILD(DT_CHOSEN(zmk_custom_led)))

#define BRT_MIN 10
#define OFF_DELAY_MS 3000

/* Fade configs */
#define FADE_STEP_MS 20
#define FADE_STEPS 20

static struct k_work_delayable auto_off_work;
static struct k_work_delayable fade_work;
static struct k_work dpi_changed_work;
static atomic_t indicator_ready;

static uint8_t current_brt = 0;
static uint8_t target_brt = 0;
static uint8_t fade_start_brt = 0;
static int fade_step = -1;

/* ★ 对外状态：最近一次有效亮度 */
static uint8_t last_valid_brt = BRT_MIN;

/* === Immediate apply LED === */
static void apply_led(uint8_t brightness) {
    if (!device_is_ready(led_dev))
        return;

    for (int i = 0; i < LED_NUM; i++) {
        led_set_brightness(led_dev, i, brightness);
    }
    current_brt = brightness;
}

/* === Fade animation handler === */
static void fade_handler(struct k_work *work) {
    if (fade_step < 0)
        return;

    float ratio = (float)fade_step / FADE_STEPS;
    int new_level = fade_start_brt + (int)((target_brt - fade_start_brt) * ratio);

    apply_led(new_level);

    fade_step++;
    if (fade_step > FADE_STEPS) {
        apply_led(target_brt);
        fade_step = -1;
        return;
    }

    k_work_reschedule(&fade_work, K_MSEC(FADE_STEP_MS));
}

/* === Start fade === */
static void fade_to(uint8_t new_brt) {
    fade_start_brt = current_brt;
    target_brt = new_brt;
    fade_step = 0;
    k_work_reschedule(&fade_work, K_MSEC(FADE_STEP_MS));
}

/* === Auto-off timeout → fade-out === */
static void auto_off_handler(struct k_work *work) {
    LOG_INF("Auto-off → fade-out to 0");
    fade_to(0);
}

static void dpi_changed_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    uint8_t led_level = MAX(BRT_MIN, sofle_trackpoint_dpi_get());
    last_valid_brt = led_level;

    if (current_brt == 0) {
        LOG_INF("TrackPoint DPI LED fade-in to %d", led_level);
        fade_to(led_level);
    } else {
        k_work_cancel_delayable(&fade_work);
        fade_step = -1;
        apply_led(led_level);
    }

    k_work_reschedule(&auto_off_work, K_MSEC(OFF_DELAY_MS));
}

/* === Public API === */
uint8_t custom_led_get_last_valid_brightness(void) { return last_valid_brt; }

void sofle_trackpoint_dpi_changed(void) {
    if (atomic_get(&indicator_ready)) {
        k_work_submit(&dpi_changed_work);
    }
}

#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPOINT_CONTROL)

/*
 * The TrackPoint module owns the DPI value now, and it is the half that gets
 * told when Studio or the &DPI keys change it (directly or over the split
 * relay), so follow its event instead of only the local driver.
 */
static int trackpoint_dpi_listener(const zmk_event_t *eh) {
    if (as_zitaotech_trackpoint_dpi_changed(eh) != NULL) {
        sofle_trackpoint_dpi_changed();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(custom_led_trackpoint_dpi, trackpoint_dpi_listener);
ZMK_SUBSCRIPTION(custom_led_trackpoint_dpi, zitaotech_trackpoint_dpi_changed);

#endif // IS_ENABLED(CONFIG_ZITAOTECH_TRACKPOINT_CONTROL)

/* === Init === */
static int init_led_follow(void) {
    if (!device_is_ready(led_dev))
        return -ENODEV;

    k_work_init_delayable(&auto_off_work, auto_off_handler);
    k_work_init_delayable(&fade_work, fade_handler);
    k_work_init(&dpi_changed_work, dpi_changed_work_handler);

    uint8_t led_level = MAX(BRT_MIN, sofle_trackpoint_dpi_get());
    last_valid_brt = led_level;

    apply_led(led_level);
    atomic_set(&indicator_ready, 1);
    k_work_reschedule(&auto_off_work, K_MSEC(OFF_DELAY_MS));

    LOG_INF("TrackPoint DPI LED initialized at %d", led_level);
    return 0;
}

SYS_INIT(init_led_follow, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
