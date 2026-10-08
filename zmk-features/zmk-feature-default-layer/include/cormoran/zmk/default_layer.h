/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Value encoding shared with the Studio protocol:
 *   >= 0  a keymap layer index,
 *   -1    leave this endpoint alone (the immutable global default applies),
 *   -2    endpoints only: resolve through the per-OS mapping.
 */
#define ZMK_DEFAULT_LAYER_UNSET (-1)
#define ZMK_DEFAULT_LAYER_VIA_OS (-2)

/* One entry per zmk_os in zmk-feature-os-detection (0 = unknown). */
#define ZMK_DEFAULT_LAYER_OS_COUNT 6

#ifdef __cplusplus
extern "C" {
#endif

/** Number of endpoint instances ZMK knows about (index range for the setters). */
uint32_t zmk_default_layer_endpoint_count(void);

/** Raw stored value for an endpoint, sentinels included. */
int32_t zmk_default_layer_endpoint_value(uint32_t endpoint_index);

/** Raw stored value for one OS entry of the mapping. */
int32_t zmk_default_layer_os_value(uint32_t os);

/** Layer an endpoint actually ends up on (sentinels resolved). */
int32_t zmk_default_layer_resolved(uint32_t endpoint_index);

/**
 * Store a value and re-apply the layer if it affects the endpoint in use.
 * Returns -EINVAL for an index or value outside the range.
 */
int zmk_default_layer_set_endpoint_value(uint32_t endpoint_index, int32_t value);
int zmk_default_layer_set_os_value(uint32_t os, int32_t value);

/**
 * Describe an endpoint index for the Studio list: whether it is the USB port,
 * and for a BLE endpoint which profile it is.  Returns false for the "nothing
 * connected" slot and for indices that are not endpoints.
 */
bool zmk_default_layer_endpoint_info(uint32_t endpoint_index, bool *is_usb,
                                     uint32_t *ble_profile_index);

/** Index of the endpoint in use, or -1 when nothing is connected. */
int32_t zmk_default_layer_active_endpoint_index(void);

/** The OS detection result the Studio state reports. */
uint32_t zmk_default_layer_current_os(void);

#ifdef __cplusplus
}
#endif
