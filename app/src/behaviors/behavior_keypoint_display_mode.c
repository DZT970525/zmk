/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT zmk_behavior_keypoint_display_mode
#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#if (IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN)) && IS_ENABLED(CONFIG_NICE_VIEW_WIDGET_STATUS) && !IS_ENABLED(CONFIG_ZMK_SETTINGS_RESET_ON_START)
#include <zmk/keypoint_quota.h>
#endif
static int pressed(struct zmk_behavior_binding *binding,struct zmk_behavior_binding_event event){
 ARG_UNUSED(binding);ARG_UNUSED(event);
#if (IS_ENABLED(CONFIG_ZMK_KEYPOINT_RELAY_SOURCE) || IS_ENABLED(CONFIG_ZITAOTECH_KEYPOINT_SCREEN)) && IS_ENABLED(CONFIG_NICE_VIEW_WIDGET_STATUS) && !IS_ENABLED(CONFIG_ZMK_SETTINGS_RESET_ON_START)
 zmk_keypoint_quota_cycle();
#endif
 return ZMK_BEHAVIOR_OPAQUE;
}
static int released(struct zmk_behavior_binding *binding,struct zmk_behavior_binding_event event){ARG_UNUSED(binding);ARG_UNUSED(event);return ZMK_BEHAVIOR_OPAQUE;}
static const struct behavior_driver_api api={.binding_pressed=pressed,.binding_released=released,.locality=BEHAVIOR_LOCALITY_EVENT_SOURCE,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
 .get_parameter_metadata=zmk_behavior_get_empty_param_metadata,
#endif
};
#define DISPLAY_INST(n) BEHAVIOR_DT_INST_DEFINE(n,NULL,NULL,NULL,NULL,POST_KERNEL,CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,&api);
DT_INST_FOREACH_STATUS_OKAY(DISPLAY_INST)
