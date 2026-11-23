/*
 * Copyright (c) 2023 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>

#include <zmk/settings.h>

// Reset before main() loads saved settings to
// ensure normal startup sees cleared settings. Bootmagic may run first.
static int reset_result = -1;

int zmk_settings_reset_result(void) { return reset_result; }

static int reset_on_start(void) {
    reset_result = zmk_settings_erase();
    return reset_result;
}

#if IS_ENABLED(CONFIG_ZITAOTECH_BOOTMAGIC)
/* Check the power-up key at APPLICATION/95 before slow flash erasure.
 * main() loads settings later, so normal reset startup still sees empty settings. */
SYS_INIT(reset_on_start, APPLICATION, 96);
#else
SYS_INIT(reset_on_start, POST_KERNEL, CONFIG_ZMK_SETTINGS_RESET_ON_START_INIT_PRIORITY);
#endif
