#include <zmk/keypoint_relay.h>
extern void mac_test_draw_receiver_prompt(void);
#include "receiver_screen.h"
#include "widgets/theme.h"
/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "custom_status_screen.h"
#include "widgets/battery_status.h"
#include "widgets/output_status.h"
#include "widgets/splash.h"
#include "widgets/snake.h"
#include "widgets/helpers/display.h"
#include "widgets/action_button.h"
#include "widgets/logo.h"
#include "widgets/configuration.h"
#include "widgets/wpm.h"
#include "widgets/modifier.h"
#include "widgets/layer_status.h"
#include <zmk/activity.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/display.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define SPLASH_DURATION_MS 50
#define SPLASH_HOLD_MS 2500

static int64_t splash_visible_at = -1;
static bool splash_finished = false;

static void show_pair_prompt(void) {
    stop_wpm_status();stop_modifier_status();stop_output_status();stop_battery_status();
    stop_animation();stop_layer_status();mac_test_draw_receiver_prompt();
    receiver_backlight_boot_ready();
}
static void pair_prompt_poll(lv_timer_t *timer) {
    ARG_UNUSED(timer);static bool previous;
    bool pending=zmk_mac_pair_pending();
    if(pending!=previous){previous=pending;receiver_screen_set_idle(false);}
    static int previous_product=-1;
    int product=zmk_keypoint_relay_connected()?zmk_keypoint_relay_status().product:0;
    if(product!=previous_product && splash_finished && !pending && !receiver_screen_is_idle()) {
        previous_product=product;print_receiver_title();
    }
}

/* === 阶段1：开机阶段 === */
void timer_splash(lv_timer_t *timer) {
    if (splash_finished) return;
    if(zmk_mac_pair_pending()) {
        show_pair_prompt();splash_finished=true;lv_timer_pause(timer);
        receiver_screen_boot_complete();return;
    }

    if (splash_visible_at < 0) {
        print_splash();
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER)
        receiver_backlight_boot_ready();
#endif
        /* Loading/decoding does not consume the visible 2.5-second hold. */
        splash_visible_at = k_uptime_get();
        return;
    }
    if (k_uptime_get() - splash_visible_at < SPLASH_HOLD_MS) return;
    LOG_INF("Splash finished -> show menu");
    initialize_battery_status();
    print_menu();
    lv_timer_pause(timer);
    splash_finished = true;
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER)
    receiver_screen_boot_complete();
#endif
}

/* === 阶段2：监听活动状态变化 === */
static atomic_t pending_activity;
static void activity_render_work(struct k_work *work) {
    ARG_UNUSED(work);
    enum zmk_activity_state state = atomic_get(&pending_activity);

    if(zmk_mac_pair_pending()){show_pair_prompt();return;}
    /* Do not let the initial ACTIVE event skip the 2.5 second boot splash. */
    if (!splash_finished)
        return;

    if (state == ZMK_ACTIVITY_ACTIVE) {
        LOG_INF("Keyboard active → show menu");
        initialize_battery_status();
        print_menu();
    } else {
        LOG_INF("Keyboard idle → show splash");
        // Stop every widget before showing the splash. Each widget redraws
        // asynchronously from its own event listener (BLE/USB, battery, wpm,
        // layer, ...). If they stay "running" while the splash is shown, an
        // incoming event will paint a connection sign or battery value on top
        // of the splash. Clearing the flags keeps the idle screen clean.
        stop_wpm_status();
        stop_modifier_status();
        stop_output_status();
        stop_battery_status();
        stop_animation();
        stop_layer_status();
        invalidate_splash();
        print_splash();
    }

}
K_WORK_DEFINE(activity_render, activity_render_work);
void receiver_screen_set_idle(bool idle) {
    atomic_set(&pending_activity, idle ? ZMK_ACTIVITY_IDLE : ZMK_ACTIVITY_ACTIVE);
    if (zmk_display_is_initialized()) k_work_submit_to_queue(zmk_display_work_q(), &activity_render);
}

static int activity_listener_cb(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER)
    return ZMK_EV_EVENT_BUBBLE;
#endif
    const struct zmk_activity_state_changed *event = as_zmk_activity_state_changed(eh);
    if (event) {
        atomic_set(&pending_activity, event->state);
        /* Full SPI redraw must never block the system queue receiving HID. */
        if (zmk_display_is_initialized())
            k_work_submit_to_queue(zmk_display_work_q(), &activity_render);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(custom_status_activity_listener, activity_listener_cb);
ZMK_SUBSCRIPTION(custom_status_activity_listener, zmk_activity_state_changed);

/* === 显示初始化 === */
lv_obj_t *zmk_display_status_screen() {
    configure();
    init_display();
    theme_init();

    zmk_widget_splash_init();
    zmk_widget_output_status_init();
    zmk_widget_peripheral_battery_status_init();
    zmk_widget_layer_init();
    zmk_widget_action_button_init();
    zmk_widget_wpm_init();
    zmk_widget_modifier_init();

    // 启动开机 splash 动画
    lv_timer_create(timer_splash, SPLASH_DURATION_MS, NULL);
    lv_timer_create(pair_prompt_poll,100,NULL);

    lv_obj_t *screen = lv_obj_create(NULL);
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER)
    /* Widgets/JPEG own direct SPI drawing. An empty LVGL screen must not
     * flush its black background over a completed splash or status frame. */
    lv_display_t *display = lv_display_get_default();
    lv_display_enable_invalidation(display, false);
    lv_display_delete_refr_timer(display);
#endif
    return screen;
}
