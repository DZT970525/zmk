/*
 * keyboard_backlight_peripheral.c - 副手键盘背光开机渐亮 + 按键触发渐亮/渐暗
 *                                    副手按键不会触发主手背光
 *                                    自行统计 WPM 动态延迟
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>
#include <zmk/events/position_state_changed.h>

LOG_MODULE_REGISTER(keyboard_backlight, CONFIG_ZMK_LOG_LEVEL);

/* ==== 获取背光设备 ==== */
#define KEYBOARD_BACKLIGHT_NODE DT_NODELABEL(keyboard_backlight)
#if !DT_NODE_HAS_STATUS(KEYBOARD_BACKLIGHT_NODE, okay)
#error "Missing DT node: keyboard_backlight"
#endif

static const struct device *const backlight_dev = DEVICE_DT_GET(KEYBOARD_BACKLIGHT_NODE);

/* ==== 渐变参数 ==== */
#define STEP 2
#define INTERVAL_MS 80
#define MAX_BRT 20
#define MIN_BRT 0
#define AUTO_OFF_DELAY_MIN_MS 1000
#define AUTO_OFF_DELAY_MAX_MS 3000
#define BOOT_FADE_DELAY_MS 3000
#define KEY_PRESS_MULT 2

/* ==== WPM 统计参数 ==== */
#define CHARS_PER_WORD 5.0
#define WPM_UPDATE_INTERVAL_SECONDS 1
#define WPM_RESET_INTERVAL_SECONDS 5

/* ==== 内部控制变量 ==== */
static int current_brt = 0;
static int pressed_key_count = 0; // 当前按下的按键数量
static bool fading_up = false;
static bool fading_down = false;
static float fade_step = STEP;

/* ==== WPM 控制变量 ==== */
static uint32_t key_pressed_count = 0;
static uint8_t wpm_state = 0;
static uint8_t wpm_update_counter = 0;

/* ==== 工作队列 ==== */
static struct k_work_delayable fade_work;
static struct k_work_delayable auto_off_work;
static struct k_work_delayable boot_delay_work;
static struct k_work_delayable wpm_work;

/* ==== 设置亮度 ==== */
static void update_brightness(int brt) {
    if (!device_is_ready(backlight_dev))
        return;
    if (brt < MIN_BRT)
        brt = MIN_BRT;
    if (brt > MAX_BRT)
        brt = MAX_BRT;
    led_set_brightness(backlight_dev, 0, brt);
    current_brt = brt;
}

/* ==== 渐亮/渐暗任务 ==== */
static void fade_handler(struct k_work *work_item) {
    bool need_reschedule = false;

    if (fading_up) {
        current_brt += (int)fade_step;
        if (current_brt >= MAX_BRT) {
            current_brt = MAX_BRT;
            fading_up = false;
        } else {
            need_reschedule = true;
        }
        update_brightness(current_brt);
    } else if (fading_down) {
        current_brt -= STEP;
        if (current_brt <= MIN_BRT) {
            current_brt = MIN_BRT;
            fading_down = false;
        } else {
            need_reschedule = true;
        }
        update_brightness(current_brt);
    }

    if (need_reschedule) {
        k_work_reschedule(&fade_work, K_MSEC(INTERVAL_MS));
    }
}

/* ==== 松开后延迟关闭任务 ==== */
static void auto_off_handler(struct k_work *work_item) {
    fading_down = true;
    fading_up = false;
    k_work_reschedule(&fade_work, K_NO_WAIT);
}

/* ==== 开机延迟检查任务 ==== */
static void boot_delay_handler(struct k_work *work_item) {
    if (current_brt == MIN_BRT)
        return;
    fading_down = true;
    fading_up = false;
    k_work_reschedule(&fade_work, K_NO_WAIT);
}

/* ==== WPM 计算任务 ==== */
static void wpm_work_handler(struct k_work *work_item) {
    wpm_update_counter++;
    wpm_state = (key_pressed_count / CHARS_PER_WORD) /
                (wpm_update_counter * WPM_UPDATE_INTERVAL_SECONDS / 60.0);
    if (wpm_update_counter >= WPM_RESET_INTERVAL_SECONDS) {
        wpm_update_counter = 0;
        key_pressed_count = 0;
    }
    k_work_schedule(work_item, K_SECONDS(WPM_UPDATE_INTERVAL_SECONDS));
}

/* ==== 键盘事件监听 ==== */
static int kb_listener_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (!ev)
        return 0;

    if (ev->source != ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL)
        return 0;

    if (ev->state) { // 按下
        pressed_key_count++;
        key_pressed_count++;
        fading_up = true;
        fading_down = false;
        fade_step = STEP * KEY_PRESS_MULT;
        k_work_reschedule(&fade_work, K_NO_WAIT);
        k_work_cancel_delayable(&auto_off_work);
    } else { // 松开
        if (pressed_key_count > 0)
            pressed_key_count--;

        if (pressed_key_count == 0) { // 最后一个键松开
            fading_up = false;
            fading_down = false; // 先停掉渐亮/渐暗
            fade_step = STEP;
            // 根据 WPM 动态延迟
            int delay_ms =
                AUTO_OFF_DELAY_MIN_MS + ((AUTO_OFF_DELAY_MAX_MS - AUTO_OFF_DELAY_MIN_MS) *
                                         (wpm_state > 100 ? 100 : wpm_state) / 100);
            k_work_schedule(&auto_off_work, K_MSEC(delay_ms));
        }
    }
    return 0;
}

ZMK_LISTENER(keyboard_backlight_listener, kb_listener_cb);
ZMK_SUBSCRIPTION(keyboard_backlight_listener, zmk_position_state_changed);

/* ==== 初始化 ==== */
static int keyboard_backlight_init(void) {
    if (!device_is_ready(backlight_dev)) {
        LOG_ERR("Keyboard backlight device not ready");
        return -ENODEV;
    }

    current_brt = MIN_BRT;
    pressed_key_count = 0;
    key_pressed_count = 0;
    wpm_state = 0;
    wpm_update_counter = 0;

    update_brightness(current_brt);

    k_work_init_delayable(&fade_work, fade_handler);
    k_work_init_delayable(&auto_off_work, auto_off_handler);
    k_work_init_delayable(&boot_delay_work, boot_delay_handler);
    k_work_init_delayable(&wpm_work, wpm_work_handler);

    // 开机渐亮
    fading_up = true;
    k_work_reschedule(&fade_work, K_NO_WAIT);

    // 开机延迟检查
    k_work_schedule(&boot_delay_work, K_MSEC(BOOT_FADE_DELAY_MS));

    // 启动 WPM 统计工作
    k_work_schedule(&wpm_work, K_SECONDS(WPM_UPDATE_INTERVAL_SECONDS));

    LOG_INF("Peripheral keyboard backlight initialized");

    return 0;
}

SYS_INIT(keyboard_backlight_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
