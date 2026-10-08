#define DT_DRV_COMPAT zmk_behavior_force_layer
#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
extern int zmk_keymap_force_layer_begin(uint32_t layer);
extern void zmk_keymap_force_layer_end(uint32_t layer);
static int force_pressed(struct zmk_behavior_binding *binding,
                         struct zmk_behavior_binding_event event) {
    return zmk_keymap_force_layer_begin(binding->param1);
}
static int force_released(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    zmk_keymap_force_layer_end(binding->param1);
    return 0;
}
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata values[] = {{
    .display_name = "Layer", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_LAYER_ID,
}};
static const struct behavior_parameter_metadata_set sets[] = {{
    .param1_values = values, .param1_values_len = ARRAY_SIZE(values),
}};
static const struct behavior_parameter_metadata metadata = {
    .sets = sets, .sets_len = ARRAY_SIZE(sets),
};
#endif
static const struct behavior_driver_api api = {
    .binding_pressed = force_pressed,
    .binding_released = force_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};
#define FORCE_INST(n) BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);
DT_INST_FOREACH_STATUS_OKAY(FORCE_INST)

#endif
