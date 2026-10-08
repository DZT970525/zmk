/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/sensors.h>

#define ZMK_RUNTIME_SENSOR_ROTATE_MAX_LAYERS ZMK_KEYMAP_LAYERS_LEN
#define ZMK_RUNTIME_SENSOR_ROTATE_MAX_SENSORS ZMK_KEYMAP_SENSORS_LEN

struct runtime_sensor_rotate_binding {
    zmk_behavior_local_id_t behavior_local_id;
    uint32_t param1;
    uint32_t param2;
    uint32_t tap_ms;
};

struct runtime_sensor_rotate_layer_bindings {
    struct runtime_sensor_rotate_binding cw_binding;
    struct runtime_sensor_rotate_binding ccw_binding;
};

/**
 * Get the layer bindings for a specific sensor and layer
 */
int zmk_runtime_sensor_rotate_get_layer_bindings(
    uint8_t sensor_index, uint8_t layer, struct runtime_sensor_rotate_layer_bindings *bindings);

/**
 * Set the layer bindings for a specific sensor and layer
 */
int zmk_runtime_sensor_rotate_set_layer_bindings(
    uint8_t sensor_index, uint8_t layer,
    const struct runtime_sensor_rotate_layer_bindings *bindings);

/**
 * Get the layer bindings for a specific sensor and layer
 */
int zmk_runtime_sensor_rotate_get_bindings(uint8_t sensor_index, uint8_t layer_index,
                                           struct runtime_sensor_rotate_layer_bindings *out);

/**
 * Get all layer bindings for a specific sensor
 */
int zmk_runtime_sensor_rotate_get_all_layer_bindings(
    uint8_t sensor_index, uint8_t max_layers,
    struct runtime_sensor_rotate_layer_bindings *bindings_array, uint8_t *actual_layers);
