/*
 * Trackpad mode switch behavior (&TP)
 *
 * Toggles the base trackpad mode between pointer mode and direction mode from a
 * keymap key (the Q20 keymap puts it on layer 2's X key).
 *
 * The key is inert until the mode-switch key is enabled in ZitaoTech Studio, so
 * a keymap that carries the binding is safe: nothing changes the mode until the
 * user asks for it on the Trackpad page.
 *
 * Copyright (c) 2026 ZitaoTech
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_trackpad_mode

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
#include <zitaotech/zmk/trackpad.h>
#endif

LOG_MODULE_REGISTER(behavior_trackpad_mode, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* Command codes.  The board keymap mirrors these (see zitaotech_q20.keymap). */
#define TP_MODE_TOGGLE_CMD 0

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata mode_values[] = {
    {
        .display_name = "Toggle pointer / direction mode",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = TP_MODE_TOGGLE_CMD,
    },
};

static const struct behavior_parameter_metadata_set metadata_set = {
    .param1_values = mode_values,
    .param1_values_len = ARRAY_SIZE(mode_values),
};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = 1,
    .sets = &metadata_set,
};

#endif /* CONFIG_ZMK_BEHAVIOR_METADATA */

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    if (binding->param1 != TP_MODE_TOGGLE_CMD) {
        LOG_ERR("Unknown trackpad mode command: %u", binding->param1);
        return -ENOTSUP;
    }

#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    if (zitaotech_trackpad_toggle_mode() < 0) {
        /* Not an error: the key is assigned but switched off in Studio. */
        LOG_DBG("Trackpad mode switch key is disabled in ZitaoTech Studio");
    }
#else
    LOG_WRN("Trackpad control module is not built in");
#endif

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_trackpad_mode_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif /* CONFIG_ZMK_BEHAVIOR_METADATA */
};

#define TRACKPAD_MODE_INST(n)                                                                      \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_trackpad_mode_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TRACKPAD_MODE_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
