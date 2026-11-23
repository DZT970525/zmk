/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT zmk_behavior_keypoint_connection_mode
#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/split/role.h>
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
#include <zmk/keypoint_relay.h>
#endif
static int pressed(struct zmk_behavior_binding *binding,
                   struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);ARG_UNUSED(event);
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE)
    zmk_keypoint_relay_toggle_route();
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_RUNTIME_ROLE)
    /* EVENT_SOURCE executes on LEFT even while DONGLE is the master. The
     * permission gate is enforced by the authoritative runtime role API. */
    zmk_split_role_toggle();
#endif
    return ZMK_BEHAVIOR_OPAQUE;
}
static int released(struct zmk_behavior_binding *binding,
                    struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);ARG_UNUSED(event);return ZMK_BEHAVIOR_OPAQUE;
}
static const struct behavior_driver_api api={
    .binding_pressed=pressed,.binding_released=released,
    .locality=BEHAVIOR_LOCALITY_EVENT_SOURCE,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata=zmk_behavior_get_empty_param_metadata,
#endif
};
#define CONNECTION_INST(n) \
    BEHAVIOR_DT_INST_DEFINE(n,NULL,NULL,NULL,NULL,POST_KERNEL, \
        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,&api);
DT_INST_FOREACH_STATUS_OKAY(CONNECTION_INST)
