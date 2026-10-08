/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hook points for the temporary layer input processor (`&zip_temp_layer`),
 * which is normally configured entirely from devicetree.  Boards that want to
 * control such a layer at runtime - ZitaoTech's automatic mouse layer, which
 * ZitaoTech Studio can switch off and whose timeout the user can adjust -
 * override the weak defaults in the core driver.
 *
 * `layer` is the layer index the input processor was bound to in devicetree,
 * and `dev` is the input device the event came from - one instance of the
 * processor can be shared by several pointing devices, and each may want its
 * own settings (ZitaoTech's trackpad and TrackPoint do).
 */

/** Whether the layer may be activated at all right now. */
bool zmk_input_processor_temp_layer_enabled(const struct device *dev, uint32_t layer);

/**
 * How long the layer stays active after the last input event.  `timeout_ms` is
 * the devicetree value; return it unchanged to keep the static behaviour.
 */
uint32_t zmk_input_processor_temp_layer_timeout_ms(const struct device *dev, uint32_t layer,
                                                   uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
