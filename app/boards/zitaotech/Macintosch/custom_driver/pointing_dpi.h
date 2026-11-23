/*
 * Copyright (c) 2026 ZitaoTech
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* Value 0 is reserved for the empty placeholder shown in ZMK Studio, so that a
 * freshly assigned DPI binding has nothing preselected and the user has to pick
 * both the device and the direction explicitly.
 */
#define DPI_DEVICE_NONE 0
#define DPI_DEVICE_TRACKPAD 1
#define DPI_DEVICE_TRACKPOINT 2

#define DPI_DIRECTION_NONE 0
#define DPI_DIRECTION_INCREASE 1
#define DPI_DIRECTION_DECREASE 2

uint8_t sofle_touchpad_dpi_get(void);
uint8_t sofle_trackpoint_dpi_get(void);

/* Optional device-specific feedback hooks. */
void sofle_touchpad_dpi_changed(void);
void sofle_trackpoint_dpi_changed(void);
