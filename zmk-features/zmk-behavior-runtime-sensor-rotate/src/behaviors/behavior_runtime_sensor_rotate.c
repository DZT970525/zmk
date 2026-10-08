/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_runtime_sensor_rotate

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/sensors.h>
#include <zmk/behavior_queue.h>
#include <zmk/virtual_key_position.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/behaviors/runtime_sensor_rotate.h>

#include <string.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct behavior_runtime_sensor_rotate_config {
    const char *default_cw_binding_name;
    const char *default_ccw_binding_name;
    struct runtime_sensor_rotate_binding default_cw_binding_params;
    struct runtime_sensor_rotate_binding default_ccw_binding_params;
};

struct behavior_runtime_sensor_rotate_data {
    struct sensor_value remainder[ZMK_RUNTIME_SENSOR_ROTATE_MAX_SENSORS][ZMK_KEYMAP_LAYERS_LEN];
    int triggers[ZMK_RUNTIME_SENSOR_ROTATE_MAX_SENSORS][ZMK_KEYMAP_LAYERS_LEN];
    struct runtime_sensor_rotate_layer_bindings bindings[ZMK_RUNTIME_SENSOR_ROTATE_MAX_SENSORS]
                                                        [ZMK_RUNTIME_SENSOR_ROTATE_MAX_LAYERS];
    bool data_accepted[ZMK_RUNTIME_SENSOR_ROTATE_MAX_SENSORS][ZMK_KEYMAP_LAYERS_LEN];
};

static struct behavior_runtime_sensor_rotate_data global_data = {};

#if ZMK_KEYMAP_HAS_SENSORS

#define _TRANSFORM_SENSOR_ENTRY(idx, layer)                                                        \
    COND_CODE_1(DT_NODE_HAS_COMPAT(DT_PHANDLE_BY_IDX(layer, sensor_bindings, idx),                 \
                                   zmk_behavior_runtime_sensor_rotate),                            \
                (DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(layer, sensor_bindings, idx))), (NULL))

#define SENSOR_LAYER(node)                                                                         \
    COND_CODE_1(                                                                                   \
        DT_NODE_HAS_PROP(node, sensor_bindings),                                                   \
        ({LISTIFY(DT_PROP_LEN(node, sensor_bindings), _TRANSFORM_SENSOR_ENTRY, (, ), node)}),      \
        ({}))

// TODO: The dimension order here is [layer][sensor]. Different from global_data which is
// [sensor][layer].
static const char *global_default_behavior_dev[ZMK_KEYMAP_LAYERS_LEN][ZMK_KEYMAP_SENSORS_LEN] = {
    DT_FOREACH_CHILD_SEP(DT_INST(0, zmk_keymap), SENSOR_LAYER, (, ))};
#endif

// Settings storage key
#define SETTINGS_KEY "rsr"

static int settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    int rc;
    int sensor_index, layer;

    // Parse key format: s<sensor_index>/l<layer>
    // Example: "s0/l1" for sensor 0, layer 1
    if (sscanf(name, "s%d/l%d", &sensor_index, &layer) == 2) {
        if (sensor_index < 0 || sensor_index >= ZMK_RUNTIME_SENSOR_ROTATE_MAX_SENSORS) {
            LOG_WRN("Invalid sensor index in settings: %d", sensor_index);
            return -EINVAL;
        }
        if (layer < 0 || layer >= ZMK_RUNTIME_SENSOR_ROTATE_MAX_LAYERS) {
            LOG_WRN("Invalid layer in settings: %d", layer);
            return -EINVAL;
        }

        if (len != sizeof(struct runtime_sensor_rotate_layer_bindings)) {
            LOG_ERR("Invalid settings data size for s%d/l%d: %d vs %d", sensor_index, layer, len,
                    sizeof(struct runtime_sensor_rotate_layer_bindings));
            return -EINVAL;
        }

        rc = read_cb(cb_arg, &global_data.bindings[sensor_index][layer],
                     sizeof(struct runtime_sensor_rotate_layer_bindings));
        if (rc < 0) {
            LOG_ERR("Failed to read settings for s%d/l%d: %d", sensor_index, layer, rc);
            return rc;
        }

        LOG_DBG("Loaded bindings for sensor %d layer %d", sensor_index, layer);
        return 0;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(behavior_runtime_sensor_rotate, SETTINGS_KEY, NULL, settings_set,
                               NULL, NULL);

int zmk_runtime_sensor_rotate_get_layer_bindings(
    uint8_t sensor_index, uint8_t layer, struct runtime_sensor_rotate_layer_bindings *bindings) {

    if (sensor_index >= ZMK_RUNTIME_SENSOR_ROTATE_MAX_SENSORS) {
        return -EINVAL;
    }
    if (layer >= ZMK_RUNTIME_SENSOR_ROTATE_MAX_LAYERS) {
        return -EINVAL;
    }

    *bindings = global_data.bindings[sensor_index][layer];
    return 0;
}

int zmk_runtime_sensor_rotate_set_layer_bindings(
    uint8_t sensor_index, uint8_t layer,
    const struct runtime_sensor_rotate_layer_bindings *bindings) {

    if (sensor_index >= ZMK_RUNTIME_SENSOR_ROTATE_MAX_SENSORS) {
        return -EINVAL;
    }
    if (layer >= ZMK_RUNTIME_SENSOR_ROTATE_MAX_LAYERS) {
        return -EINVAL;
    }

    global_data.bindings[sensor_index][layer] = *bindings;

    // Save to settings with per-sensor, per-layer key
    char key[32];
    snprintf(key, sizeof(key), SETTINGS_KEY "/s%d/l%d", sensor_index, layer);

    int rc = settings_save_one(key, &global_data.bindings[sensor_index][layer],
                               sizeof(struct runtime_sensor_rotate_layer_bindings));
    if (rc != 0) {
        LOG_ERR("Failed to save settings for sensor %d layer %d: %d", sensor_index, layer, rc);
        return rc;
    }

    LOG_DBG("Saved bindings (local_id=%d) for sensor %d layer %d",
            bindings->cw_binding.behavior_local_id, sensor_index, layer);
    return 0;
}

int zmk_runtime_sensor_rotate_get_all_layer_bindings(
    uint8_t sensor_index, uint8_t max_layers,
    struct runtime_sensor_rotate_layer_bindings *bindings_array, uint8_t *actual_layers) {

    if (sensor_index >= ZMK_RUNTIME_SENSOR_ROTATE_MAX_SENSORS) {
        return -EINVAL;
    }

    uint8_t layers = ZMK_RUNTIME_SENSOR_ROTATE_MAX_LAYERS;
    if (max_layers < layers) {
        layers = max_layers;
    }

    for (uint8_t i = 0; i < layers; i++) {
        zmk_runtime_sensor_rotate_get_bindings(sensor_index, i, &bindings_array[i]);
    }

    if (actual_layers) {
        *actual_layers = layers;
    }

    return 0;
}

int zmk_runtime_sensor_rotate_get_bindings(uint8_t sensor_index, uint8_t layer_index,
                                           struct runtime_sensor_rotate_layer_bindings *out) {
    if (sensor_index >= ZMK_RUNTIME_SENSOR_ROTATE_MAX_SENSORS) {
        return -EINVAL;
    }
    if (layer_index >= ZMK_RUNTIME_SENSOR_ROTATE_MAX_LAYERS) {
        return -EINVAL;
    }
    // set from runtime first
    *out = global_data.bindings[sensor_index][layer_index];
    // If not set, fill from default
    if (out->cw_binding.behavior_local_id == 0 || out->ccw_binding.behavior_local_id == 0) {
#if ZMK_KEYMAP_HAS_SENSORS
        if (global_default_behavior_dev[layer_index][sensor_index] != NULL) {
            const struct device *dev =
                zmk_behavior_get_binding(global_default_behavior_dev[layer_index][sensor_index]);
            if (dev) {
                const struct behavior_runtime_sensor_rotate_config *config = dev->config;
                if (out->cw_binding.behavior_local_id == 0 &&
                    config->default_cw_binding_name != NULL) {
                    out->cw_binding.behavior_local_id =
                        zmk_behavior_get_local_id(config->default_cw_binding_name);
                    out->cw_binding.param1 = config->default_cw_binding_params.param1;
                    out->cw_binding.param2 = config->default_cw_binding_params.param2;
                    out->cw_binding.tap_ms = config->default_cw_binding_params.tap_ms;
                }
                if (out->ccw_binding.behavior_local_id == 0 &&
                    config->default_ccw_binding_name != NULL) {
                    out->ccw_binding.behavior_local_id =
                        zmk_behavior_get_local_id(config->default_ccw_binding_name);
                    out->ccw_binding.param1 = config->default_ccw_binding_params.param1;
                    out->ccw_binding.param2 = config->default_ccw_binding_params.param2;
                    out->ccw_binding.tap_ms = config->default_ccw_binding_params.tap_ms;
                }
            } else {
                LOG_ERR("Behavior device not found: %s",
                        global_default_behavior_dev[layer_index][sensor_index]);
            }
        }
#endif
    }
    return 0;
}

static int behavior_runtime_sensor_rotate_accept_data(
    struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event,
    const struct zmk_sensor_config *sensor_config, size_t channel_data_size,
    const struct zmk_sensor_channel_data *channel_data) {

    const struct sensor_value value = channel_data[0].value;
    int triggers;
    int sensor_index = ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);

    if (sensor_index >= ZMK_RUNTIME_SENSOR_ROTATE_MAX_SENSORS) {
        LOG_ERR("Sensor index %d out of bounds", sensor_index);
        return -EINVAL;
    }

    // Check if we already accepted data for this sensor/layer combination
    if (global_data.data_accepted[sensor_index][event.layer]) {
        LOG_DBG("Already accepted data for sensor %d layer %d", sensor_index, event.layer);
        return 0;
    }

    // Mark as accepted to prevent duplicate processing
    global_data.data_accepted[sensor_index][event.layer] = true;

    // Same logic as behavior_sensor_rotate_common
    if (value.val1 == 0) {
        triggers = value.val2;
    } else {
        struct sensor_value remainder = global_data.remainder[sensor_index][event.layer];

        remainder.val1 += value.val1;
        remainder.val2 += value.val2;

        if (remainder.val2 >= 1000000 || remainder.val2 <= -1000000) {
            remainder.val1 += remainder.val2 / 1000000;
            remainder.val2 %= 1000000;
        }

        int trigger_degrees = 360 / sensor_config->triggers_per_rotation;
        triggers = remainder.val1 / trigger_degrees;
        remainder.val1 %= trigger_degrees;

        global_data.remainder[sensor_index][event.layer] = remainder;
    }

    LOG_DBG("Sensor %d layer %d: val1=%d val2=%d remainder=%d/%d triggers=%d", sensor_index,
            event.layer, value.val1, value.val2,
            global_data.remainder[sensor_index][event.layer].val1,
            global_data.remainder[sensor_index][event.layer].val2, triggers);

    global_data.triggers[sensor_index][event.layer] = triggers;
    return 0;
}

static int behavior_runtime_sensor_rotate_process(struct zmk_behavior_binding *binding,
                                                  struct zmk_behavior_binding_event event,
                                                  enum behavior_sensor_binding_process_mode mode) {

    const int sensor_index = ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);

    if (sensor_index >= ZMK_RUNTIME_SENSOR_ROTATE_MAX_SENSORS) {
        LOG_ERR("Sensor index %d out of bounds", sensor_index);
        return -EINVAL;
    }

    if (mode != BEHAVIOR_SENSOR_BINDING_PROCESS_MODE_TRIGGER) {
        // Reset triggers and accepted flag
        global_data.triggers[sensor_index][event.layer] = 0;
        global_data.data_accepted[sensor_index][event.layer] = false;
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    int triggers = global_data.triggers[sensor_index][event.layer];

    // Reset accepted flag after processing
    global_data.data_accepted[sensor_index][event.layer] = false;

    if (event.layer >= ZMK_RUNTIME_SENSOR_ROTATE_MAX_LAYERS) {
        LOG_WRN("Layer %d exceeds max layers, skipping", event.layer);
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    struct runtime_sensor_rotate_binding triggered_binding_data;
    // Check runtime bindings
    if (triggers > 0) {
        triggered_binding_data = global_data.bindings[sensor_index][event.layer].cw_binding;
    } else if (triggers < 0) {
        triggered_binding_data = global_data.bindings[sensor_index][event.layer].ccw_binding;
    } else {
        return ZMK_BEHAVIOR_TRANSPARENT;
    }
    const char *behavior_name = NULL;
    if (triggered_binding_data.behavior_local_id == 0) {
        // Check default bindings
        const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
        if (!dev) {
            LOG_ERR("Behavior device not found: %s", binding->behavior_dev);
            return ZMK_BEHAVIOR_TRANSPARENT;
        }
        const struct behavior_runtime_sensor_rotate_config *config = dev->config;
        if (triggers > 0 && config->default_cw_binding_name != NULL) {
            behavior_name = config->default_cw_binding_name;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
            triggered_binding_data.behavior_local_id = zmk_behavior_get_local_id(behavior_name);
#endif
            triggered_binding_data.param1 = config->default_cw_binding_params.param1;
            triggered_binding_data.param2 = config->default_cw_binding_params.param2;
            triggered_binding_data.tap_ms = config->default_cw_binding_params.tap_ms;
        } else if (triggers < 0 && config->default_ccw_binding_name != NULL) {
            behavior_name = config->default_ccw_binding_name;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
            triggered_binding_data.behavior_local_id = zmk_behavior_get_local_id(behavior_name);
#endif
            triggered_binding_data.param1 = config->default_ccw_binding_params.param1;
            triggered_binding_data.param2 = config->default_ccw_binding_params.param2;
            triggered_binding_data.tap_ms = config->default_ccw_binding_params.tap_ms;
        }
    } else {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
        // Resolve behavior name from local_id for runtime binding
        behavior_name =
            zmk_behavior_find_behavior_name_from_local_id(triggered_binding_data.behavior_local_id);
#endif
        if (!behavior_name) {
            LOG_ERR("Failed to find behavior for local_id %d",
                    triggered_binding_data.behavior_local_id);
            return ZMK_BEHAVIOR_TRANSPARENT;
        }
    }
    // Check if binding is configured
    if (triggered_binding_data.behavior_local_id == 0 && behavior_name == NULL) {
        LOG_DBG("No binding configured for sensor %d layer %d", sensor_index, event.layer);
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    // Create the zmk_behavior_binding for execution
    struct zmk_behavior_binding triggered_binding = {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
        .local_id = triggered_binding_data.behavior_local_id,
#endif
        .behavior_dev = behavior_name,
        .param1 = triggered_binding_data.param1,
        .param2 = triggered_binding_data.param2,
    };

    // TODO: optimize transparent behavior check
    if (strcmp(triggered_binding.behavior_dev, "transparent") == 0) {
        LOG_DBG("Binding is transparent behavior, skipping");
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    event.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL;
#endif

    if (triggers < 0) {
        triggers = -triggers;
    }

    for (int i = 0; i < triggers; i++) {
        zmk_behavior_queue_add(&event, triggered_binding, true, triggered_binding_data.tap_ms);
        zmk_behavior_queue_add(&event, triggered_binding, false, 0);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_runtime_sensor_rotate_driver_api = {
    .sensor_binding_accept_data = behavior_runtime_sensor_rotate_accept_data,
    .sensor_binding_process = behavior_runtime_sensor_rotate_process};

#define RUNTIME_SENSOR_ROTATE_INST(n)                                                              \
    static struct behavior_runtime_sensor_rotate_config                                            \
        behavior_runtime_sensor_rotate_config_##n = {                                              \
            .default_cw_binding_name =                                                             \
                COND_CODE_1(DT_INST_NODE_HAS_PROP(n, cw_binding),                                  \
                            (DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, cw_binding, 0))), (NULL)),   \
            .default_ccw_binding_name =                                                            \
                COND_CODE_1(DT_INST_NODE_HAS_PROP(n, ccw_binding),                                 \
                            (DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, ccw_binding, 0))), (NULL)),  \
            .default_cw_binding_params = COND_CODE_1(                                              \
                DT_INST_NODE_HAS_PROP(n, cw_binding),                                              \
                ({.param1 =                                                                        \
                      COND_CODE_1(DT_PHA_HAS_CELL_AT_IDX(DT_DRV_INST(n), cw_binding, 0, param1),   \
                                  (DT_PHA_BY_IDX(DT_DRV_INST(n), cw_binding, 0, param1)), (0)),    \
                  .param2 =                                                                        \
                      COND_CODE_1(DT_PHA_HAS_CELL_AT_IDX(DT_DRV_INST(n), cw_binding, 0, param2),   \
                                  (DT_PHA_BY_IDX(DT_DRV_INST(n), cw_binding, 0, param2)), (0)),    \
                  .tap_ms = DT_INST_PROP_OR(n, tap_ms, 5)}),                                       \
                ({})),                                                                             \
            .default_ccw_binding_params = COND_CODE_1(                                             \
                DT_INST_NODE_HAS_PROP(n, ccw_binding),                                             \
                ({.param1 =                                                                        \
                      COND_CODE_1(DT_PHA_HAS_CELL_AT_IDX(DT_DRV_INST(n), ccw_binding, 0, param1),  \
                                  (DT_PHA_BY_IDX(DT_DRV_INST(n), ccw_binding, 0, param1)), (0)),   \
                  .param2 =                                                                        \
                      COND_CODE_1(DT_PHA_HAS_CELL_AT_IDX(DT_DRV_INST(n), ccw_binding, 0, param2),  \
                                  (DT_PHA_BY_IDX(DT_DRV_INST(n), ccw_binding, 0, param2)), (0)),   \
                  .tap_ms = DT_INST_PROP_OR(n, tap_ms, 5)}),                                       \
                ({})),                                                                             \
    };                                                                                             \
                                                                                                   \
    BEHAVIOR_DT_INST_DEFINE(                                                                       \
        n, NULL, NULL, &global_data, &behavior_runtime_sensor_rotate_config_##n, POST_KERNEL,      \
        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_runtime_sensor_rotate_driver_api);

DT_INST_FOREACH_STATUS_OKAY(RUNTIME_SENSOR_ROTATE_INST)
