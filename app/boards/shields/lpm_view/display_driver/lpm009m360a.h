/*
 * Copyright (c) 2023 Taisheng WANG <wstrn66@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LPM009M360A_DISPLAY_DRIVER_H__
#define LPM009M360A_DISPLAY_DRIVER_H__

#include <zephyr/kernel.h>

#define LPM009M360A_CMD_NO_UPDATE 0x00
#define LPM009M360A_CMD_BLINKING_BLACK 0x10
#define LPM009M360A_CMD_BLINKING_INVERSION 0x14
#define LPM009M360A_CMD_BLINKING_WHITE 0x18
#define LPM009M360A_CMD_ALL_CLEAR 0x20
#define LPM009M360A_CMD_VCOM 0x40
#define LPM009M360A_CMD_UPDATE 0x80

/*
 * Data mode select bits (M3..M5) of the data update command. The value is
 * shifted into bits 2..4 of the update command, so it can be used directly as
 * the "color_mode" devicetree property.
 */
#define LPM009M360A_DATA_MODE_3BIT 0x00 /* 8 colors, R/G/B bit per dot */
#define LPM009M360A_DATA_MODE_1BIT 0x02 /* monochrome, one bit per dot */
#define LPM009M360A_DATA_MODE_4BIT 0x04 /* 8 colors, R/G/B + dummy bit per dot */

#endif /* LPM009M360A_DISPLAY_DRIVER_H__ */
