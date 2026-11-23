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

/* Safe to call from the A320 MOTION GPIO ISR. */
void indicator_tp_motion_triggered(void);

/* Safely displays the newly selected DPI from the system work queue. */
void indicator_tp_dpi_changed(void);

/*
 * Report that the trackpad just fired an arrow key.
 *
 * Direction mode blanks the trackpad LED for a moment on every arrow key,
 * while pointer mode keeps it lit while a finger moves - so a glance at the
 * LED tells the two modes apart.  The blank is applied on the indicator work
 * queue, so this function only queues it.
 */
void indicator_tp_arrow_fired(void);

#ifdef __cplusplus
}
#endif
