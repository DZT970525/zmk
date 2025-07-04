#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>

#include <zmk/activity.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_keyboard_slidebacklight),
             "keyboard_slidebacklight: No zmk_keyboard_slidebacklight chosen node found");

static const struct device *const indiled_dev =
    DEVICE_DT_GET(DT_CHOSEN(zmk_keyboard_slidebacklight));

#define CHILD_COUNT(...) +1
#define DT_NUM_CHILD(node_id) (DT_FOREACH_CHILD(node_id, CHILD_COUNT))
#define INDICATOR_LED_NUM_LEDS (DT_NUM_CHILD(DT_CHOSEN(zmk_keyboard_slidebacklight)))

#define BRT_MAX CONFIG_ZMK_KEYBOARD_BACKLIGHT_BRIGHTNESS

static struct k_work_delayable polling_work;
static bool prev_active = false;

static void set_slidebacklight_brightness(uint8_t level) {
    if (!device_is_ready(indiled_dev)) {
        LOG_ERR("Indicator LED device not ready");
        return;
    }

    for (int i = 0; i < INDICATOR_LED_NUM_LEDS; i++) {
        int err = led_set_brightness(indiled_dev, i, level);
        if (err < 0) {
            LOG_ERR("Failed to set LED[%d] brightness: %d", i, err);
        }
    }
}

static void polling_work_handler(struct k_work *work) {
    bool active = (zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);

    if (active != prev_active) {
        prev_active = active;
        set_slidebacklight_brightness(active ? BRT_MAX : 0);
        LOG_DBG("Slidebacklight state changed: active=%d", active);
    }

    k_work_reschedule(&polling_work, K_MSEC(100)); // 100ms轮询一次，可根据需要调整
}

static int slidebacklight_init(void) {
    if (!device_is_ready(indiled_dev)) {
        LOG_ERR("LED indicator device not ready");
        return -ENODEV;
    }

    prev_active = (zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    set_slidebacklight_brightness(prev_active ? BRT_MAX : 0);

    k_work_init_delayable(&polling_work, polling_work_handler);
    k_work_reschedule(&polling_work, K_MSEC(100));

    return 0;
}

SYS_INIT(slidebacklight_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
