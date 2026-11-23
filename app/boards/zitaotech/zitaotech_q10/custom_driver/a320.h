/*
 * Copyright (c) 2023 ZitaoTech
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef A320_H
#define A320_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define TP_INC_CMD 0
#define TP_DEC_CMD 1

/**
 * @brief check if the touchpad is touched
 *
 * @return true if touched
 */
bool tp_is_touched(void);

/** Increase/decrease the independent trackpad DPI setting. */
void a320_dpi_increase(void);
void a320_dpi_decrease(void);
uint8_t a320_dpi_get(void);

#ifdef __cplusplus
}
#endif

#endif // A320__0x3B_H
