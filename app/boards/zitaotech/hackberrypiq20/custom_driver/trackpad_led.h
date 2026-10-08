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

/**
 * @brief Report that the trackpad just fired an arrow key.
 *
 * Direction mode blanks the trackpad LED for a moment on every arrow key, while
 * pointer mode keeps it lit while a finger moves - so a glance at the LED tells
 * the two modes apart.  The blank is applied on the indicator work queue, so
 * this function only queues it.
 */
void indicator_tp_arrow_fired(void);

#ifdef __cplusplus
}
#endif
