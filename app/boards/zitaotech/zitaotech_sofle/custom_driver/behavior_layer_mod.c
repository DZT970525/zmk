/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT zitaotech_behavior_layer_mod
#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/events/keycode_state_changed.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
/* Emit ordinary explicit modifier events, preserving HID reference counts,
 * endpoint routing and receiver forwarding exactly like physical modifiers. */
static int send_modifiers(uint32_t mask, bool pressed, int64_t timestamp) {
    int first_error = 0;
    for (int bit = 0; bit < 8; bit++) {
        if (mask & BIT(bit)) {
            int err = raise_zmk_keycode_state_changed_from_encoded(
                0x000700e0u + bit, pressed, timestamp);
            if (err < 0 && !first_error) first_error = err;
        }
    }
    return first_error;
}
static int pressed(struct zmk_behavior_binding *binding,
                   struct zmk_behavior_binding_event event) {
    if (binding->param2 > 255) return -EINVAL;
    int err = zmk_keymap_layer_activate(binding->param1, false);
    if (err < 0) return err;
    err = send_modifiers(binding->param2, true, event.timestamp);
    if (err < 0) {
        send_modifiers(binding->param2, false, event.timestamp);
        zmk_keymap_layer_deactivate(binding->param1, false);
    }
    return err;
}
static int released(struct zmk_behavior_binding *binding,
                    struct zmk_behavior_binding_event event) {
    int err = send_modifiers(binding->param2 & 255, false, event.timestamp);
    int layer_err = zmk_keymap_layer_deactivate(binding->param1, false);
    return err < 0 ? err : layer_err;
}
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata layers[] = {
    {.display_name = "Layer", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_LAYER_ID},
};
static const struct behavior_parameter_value_metadata modifiers[] = {
    {.display_name = "Modifiers", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
     .range = {.min = 0, .max = 255}},
};
static const struct behavior_parameter_metadata_set sets[] = {{
    .param1_values = layers, .param1_values_len = ARRAY_SIZE(layers),
    .param2_values = modifiers, .param2_values_len = ARRAY_SIZE(modifiers),
}};
static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(sets), .sets = sets,
};
#endif
static const struct behavior_driver_api api = {
    .binding_pressed = pressed, .binding_released = released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};
#define LM_INST(n) BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, \
    CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);
DT_INST_FOREACH_STATUS_OKAY(LM_INST)
#endif
