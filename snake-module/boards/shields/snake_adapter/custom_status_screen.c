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

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// #include "widgets/helpers/settings.h"

#define LOGO_REFRESH_INTERVAL 500 // 每500ms重绘一次

void timer_splash(lv_timer_t *timer) {
    print_splash(); // 每次循环重绘，防止被清屏
}

lv_obj_t *zmk_display_status_screen() {
    configure();
    init_display();
    theme_init();
    logo_animation_init();
    zmk_widget_splash_init();

    // 定时刷新 splash（比如每 500ms）
    lv_timer_create(timer_splash, 500, NULL);
    print_splash();
    return lv_obj_create(NULL);
}
