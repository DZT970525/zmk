/*
 * Independent touchpad and TrackPoint DPI behavior for SOFLE.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_pointing_dpi

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include "pointing_dpi.h"
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
#include <zitaotech/zmk/trackpad.h>
#endif
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPOINT_CONTROL)
#include <zitaotech/zmk/trackpoint.h>
#endif

LOG_MODULE_REGISTER(behavior_pointing_dpi, CONFIG_ZMK_LOG_LEVEL);

#define TOUCHPAD_DPI_MIN 10
#define TRACKPOINT_DPI_MIN 10
#define POINTING_DPI_MAX 100
#define POINTING_DPI_STEP 10
#define POINTING_DPI_DEFAULT 40

#if !IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
/* Only used when the Studio trackpad module is not in charge of the pad's DPI. */
static atomic_t touchpad_dpi = ATOMIC_INIT(POINTING_DPI_DEFAULT);
#endif
#if !IS_ENABLED(CONFIG_ZITAOTECH_TRACKPOINT_CONTROL)
/* Only used when the TrackPoint module is not in charge of its DPI. */
static atomic_t trackpoint_dpi = ATOMIC_INIT(POINTING_DPI_DEFAULT);
#endif

__weak void sofle_touchpad_dpi_changed(void) {}
__weak void sofle_trackpoint_dpi_changed(void) {}

/*
 * The trackpad's DPI lives in the ZitaoTech Studio trackpad module when it is
 * enabled, so the Studio slider, the &DPI keys and the driver all agree.  The
 * TrackPoint keeps its own value, and the local trackpad value stays the
 * fallback for builds without the module.
 */
uint8_t sofle_touchpad_dpi_get(void) {
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
    return (uint8_t)zitaotech_trackpad_dpi_value();
#else
    return (uint8_t)atomic_get(&touchpad_dpi);
#endif
}
uint8_t sofle_trackpoint_dpi_get(void) {
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPOINT_CONTROL)
    /* The module owns the value so the Studio slider, the &DPI keys and the
     * TrackPoint LED on the other half all agree. */
    return (uint8_t)zitaotech_trackpoint_dpi_value();
#else
    return (uint8_t)atomic_get(&trackpoint_dpi);
#endif
}

static atomic_val_t increase_dpi(atomic_t *dpi) {
    atomic_val_t value = MIN(atomic_get(dpi) + POINTING_DPI_STEP, POINTING_DPI_MAX);
    atomic_set(dpi, value);
    return value;
}

static atomic_val_t decrease_dpi(atomic_t *dpi, atomic_val_t minimum) {
    atomic_val_t value = MAX(atomic_get(dpi) - POINTING_DPI_STEP, minimum);
    atomic_set(dpi, value);
    return value;
}

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata device_values[] = {
    {
        .display_name = "",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = DPI_DEVICE_NONE,
    },
    {
        .display_name = "Trackpad",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = DPI_DEVICE_TRACKPAD,
    },
    {
        .display_name = "TrackPoint",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = DPI_DEVICE_TRACKPOINT,
    },
};

static const struct behavior_parameter_value_metadata direction_values[] = {
    {
        .display_name = "",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = DPI_DIRECTION_NONE,
    },
    {
        .display_name = "Increase",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = DPI_DIRECTION_INCREASE,
    },
    {
        .display_name = "Decrease",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = DPI_DIRECTION_DECREASE,
    },
};

static const struct behavior_parameter_metadata_set metadata_set = {
    .param1_values = device_values,
    .param1_values_len = ARRAY_SIZE(device_values),
    .param2_values = direction_values,
    .param2_values_len = ARRAY_SIZE(direction_values),
};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = 1,
    .sets = &metadata_set,
};

#endif /* CONFIG_ZMK_BEHAVIOR_METADATA */

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    atomic_val_t dpi = 0;
    atomic_t *dpi_value = NULL;
    atomic_val_t minimum = 0;
    const char *device_name = "Pointing device";

    switch (binding->param1) {
    case DPI_DEVICE_NONE:
        /* Nothing selected yet, the binding is intentionally inert. */
        return ZMK_BEHAVIOR_OPAQUE;
    case DPI_DEVICE_TRACKPAD:
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPAD_CONTROL)
        if (binding->param2 == DPI_DIRECTION_NONE) {
            /* Nothing selected yet, the binding is intentionally inert. */
            return ZMK_BEHAVIOR_OPAQUE;
        }
        /* The trackpad module owns the touch pad's DPI steps. */
        zitaotech_trackpad_step_dpi(binding->param2 == DPI_DIRECTION_DECREASE ? -1 : 1);
        sofle_touchpad_dpi_changed();
        LOG_INF("Touchpad DPI %s to %d",
                binding->param2 == DPI_DIRECTION_DECREASE ? "decreased" : "increased",
                (int)sofle_touchpad_dpi_get());
        return ZMK_BEHAVIOR_OPAQUE;
#else
        dpi_value = &touchpad_dpi;
        minimum = TOUCHPAD_DPI_MIN;
        device_name = "Touchpad";
        break;
#endif
    case DPI_DEVICE_TRACKPOINT:
#if IS_ENABLED(CONFIG_ZITAOTECH_TRACKPOINT_CONTROL)
        if (binding->param2 == DPI_DIRECTION_NONE) {
            /* Nothing selected yet, the binding is intentionally inert. */
            return ZMK_BEHAVIOR_OPAQUE;
        }
        /* The module owns the TrackPoint's steps; it also mirrors the new value
         * to the other half (motion scaling on the central, LED on the
         * peripheral) and notifies Studio. */
        zitaotech_trackpoint_step_dpi(binding->param2 == DPI_DIRECTION_DECREASE ? -1 : 1);
        LOG_INF("TrackPoint DPI %s to %d",
                binding->param2 == DPI_DIRECTION_DECREASE ? "decreased" : "increased",
                (int)sofle_trackpoint_dpi_get());
        return ZMK_BEHAVIOR_OPAQUE;
#else
        dpi_value = &trackpoint_dpi;
        minimum = TRACKPOINT_DPI_MIN;
        device_name = "TrackPoint";
        break;
#endif
    default:
        LOG_ERR("Unknown pointing DPI device: %u", binding->param1);
        return -ENOTSUP;
    }

    switch (binding->param2) {
    case DPI_DIRECTION_NONE:
        /* Nothing selected yet, the binding is intentionally inert. */
        return ZMK_BEHAVIOR_OPAQUE;
    case DPI_DIRECTION_INCREASE:
        dpi = increase_dpi(dpi_value);
        break;
    case DPI_DIRECTION_DECREASE:
        dpi = decrease_dpi(dpi_value, minimum);
        break;
    default:
        LOG_ERR("Unknown pointing DPI direction: %u", binding->param2);
        return -ENOTSUP;
    }

    if (binding->param1 == DPI_DEVICE_TRACKPAD) {
        sofle_touchpad_dpi_changed();
    } else {
        sofle_trackpoint_dpi_changed();
    }

    LOG_INF("%s DPI %s to %d", device_name,
            binding->param2 == DPI_DIRECTION_INCREASE ? "increased" : "decreased", (int)dpi);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_pointing_dpi_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    /* The central value scales motion; the mirrored peripheral value drives its LED. */
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif /* CONFIG_ZMK_BEHAVIOR_METADATA */
};

#define POINTING_DPI_INST(n)                                                                       \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_pointing_dpi_driver_api);

DT_INST_FOREACH_STATUS_OKAY(POINTING_DPI_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
