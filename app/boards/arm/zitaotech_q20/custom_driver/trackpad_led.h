/*
 * Copyright (c) 2025 ZitaoTech
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get current indicator led brightness
 *
 * @return uint8_t valid LED brightness
 */
uint8_t indicator_tp_get_last_valid_brightness(void);

/**
 * @brief Notify the trackpad LED controller of a MOTION falling edge.
 *
 * This function is safe to call from the GPIO ISR.  The actual PWM update is
 * deferred to the system work queue because the LED driver is not ISR-safe.
 */
void indicator_tp_motion_triggered(void);

/**
 * @brief Show the current trackpad DPI on the trackpad LED immediately.
 *
 * The PWM update is deferred to the system work queue, so this function is
 * safe to call directly after changing the DPI.
 */
void indicator_tp_dpi_changed(void);

#ifdef __cplusplus
}
#endif
