/*
 * Trackpad DPI behavior
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_trackpad_dpi

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include "a320.h"
#include "trackpad_led.h"
#include <zitaotech/zmk/trackpad.h>

void a320_dpi_increase(void) {
    zitaotech_trackpad_step_dpi(1);
}
void a320_dpi_decrease(void) {
    zitaotech_trackpad_step_dpi(-1);
}

LOG_MODULE_REGISTER(behavior_trackpad_dpi, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata dpi_values[] = {
    {.display_name = "Increase trackpad DPI", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = TP_INC_CMD},
    {.display_name = "Decrease trackpad DPI", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = TP_DEC_CMD},
};
static const struct behavior_parameter_metadata_set dpi_set = {
    .param1_values = dpi_values, .param1_values_len = ARRAY_SIZE(dpi_values),
};
static const struct behavior_parameter_metadata metadata = {.sets_len = 1, .sets = &dpi_set};
#endif

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case TP_INC_CMD:
        a320_dpi_increase();
        break;
    case TP_DEC_CMD:
        a320_dpi_decrease();
        break;
    default:
        LOG_ERR("Unknown trackpad DPI command: %u", binding->param1);
        return -ENOTSUP;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_trackpad_dpi_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

#define TRACKPAD_DPI_INST(n)                                                                       \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_trackpad_dpi_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TRACKPAD_DPI_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
