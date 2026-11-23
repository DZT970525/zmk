/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/settings/settings.h>
#include <zmk/split/role.h>
#include <zmk/pointing_profiles.h>
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER)
#include <zmk/keypoint_relay.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_DISPLAY)

#include <zmk/display.h>
#include <lvgl.h>

#endif

int main(void) {
    LOG_INF("Welcome to ZMK!\n");

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_subsys_init();
#if !IS_ENABLED(CONFIG_ZMK_DIAGNOSTIC_SKIP_SETTINGS_LOAD)
    uint32_t settings_started = k_uptime_get_32();
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_ID_TYPE_SETTINGS_TABLE) && \
    IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS)
    /* Custom BEHAVIOR values validate their local IDs while loading. NVS
     * iteration order follows write history, so load/commit the ID table
     * first, before a saved combo binding can be encountered. This reads
     * only the behavior subtree and preserves every application setting. */
    int behavior_load_err = settings_load_subtree("behavior");
    if (behavior_load_err < 0) {
        LOG_ERR("Failed to preload behavior IDs: %d", behavior_load_err);
    }
#endif
    settings_load();
    LOG_INF("boot timing: full settings %u ms, finished at %u ms",
            k_uptime_get_32() - settings_started, k_uptime_get_32());
#else
    /* Preserve Bluetooth identities/bonds so diagnostic startup does not
     * create replacements. Application keymaps/settings remain unloaded. */
    settings_load_subtree("bt");
#endif
#endif

    if (zmk_pointing_profiles_start) zmk_pointing_profiles_start();
#if IS_ENABLED(CONFIG_SOFLE_MODULAR_POINTING) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    extern void sofle_extra_pointing_profiles_start(void);
    sofle_extra_pointing_profiles_start();
#endif

#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_CONFIG_SYNC) || IS_ENABLED(CONFIG_ZMK_KEYPOINT_CONFIG_MIRROR)
    zmk_keypoint_config_start();
#endif
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_RECEIVER)
    zmk_keypoint_relay_start();
#endif
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_TOPOLOGY_FALLBACK)
    zmk_keypoint_topology_start();
#endif

#ifdef CONFIG_ZMK_DISPLAY
    zmk_display_init();

#if IS_ENABLED(CONFIG_ARCH_POSIX)
    // Workaround for an SDL display issue:
    // https://github.com/zephyrproject-rtos/zephyr/issues/71410
    while (1) {
        lv_task_handler();
        k_sleep(K_MSEC(10));
    }
#endif

#endif /* CONFIG_ZMK_DISPLAY */

    return 0;
}
