/*
 * Copyright (c) 2021 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_mouse_key_toggle

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/hid.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>

#ifndef ZMK_HID_MOUSE_NUM_BUTTONS
#define ZMK_HID_MOUSE_NUM_BUTTONS 0x05
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct behavior_mouse_key_toggle_data {
    uint32_t held_buttons;
};

static int behavior_mouse_key_toggle_init(const struct device *dev) { return 0; };

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    LOG_DBG("position %d keycode 0x%02X", event.position, binding->param1);

    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_mouse_key_toggle_data *data = dev->data;

    uint32_t mask = binding->param1;
    if (!mask || (mask & ~((1U << ZMK_HID_MOUSE_NUM_BUTTONS) - 1U))) return -EINVAL;
    /* Track each button independently; one binding must not flip another. */
    for (int i = 0; i < ZMK_HID_MOUSE_NUM_BUTTONS; i++) {
        if (mask & BIT(i)) {
            bool pressed = !(data->held_buttons & BIT(i));
            int err = input_report_key(dev, INPUT_BTN_0 + i, pressed, true, K_FOREVER);
            if (err < 0) return err;
            data->held_buttons ^= BIT(i);
        }
    }

    return 0;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata values[] = {
    {.display_name="MB1", .type=BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value=MB1},
    {.display_name="MB2", .type=BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value=MB2},
    {.display_name="MB3", .type=BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value=MB3},
    {.display_name="MB4", .type=BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value=MB4},
    {.display_name="MB5", .type=BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value=MB5},
};
static const struct behavior_parameter_metadata_set sets[] = {{
    .param1_values=values, .param1_values_len=ARRAY_SIZE(values),
}};
static const struct behavior_parameter_metadata metadata = {
    .sets=sets, .sets_len=ARRAY_SIZE(sets),
};
#endif
static const struct behavior_driver_api behavior_mouse_key_toggle_driver_api = {
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
    .binding_pressed = on_keymap_binding_pressed,
};

#define MKTG_INST(n)                                                                               \
    static struct behavior_mouse_key_toggle_data behavior_mouse_key_toggle_data_##n = {};          \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_mouse_key_toggle_init, NULL,                               \
                            &behavior_mouse_key_toggle_data_##n, NULL, POST_KERNEL,                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_mouse_key_toggle_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MKTG_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
