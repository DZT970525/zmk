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

/**
 * @brief check if the touchpad is touched
 *
 * @return true if touched
 */
bool tp_is_touched(void);

/** DPI the pointer moves with (the Studio trackpad module owns it when enabled). */
uint8_t a320_dpi_get(void);

#ifdef __cplusplus
}
#endif

#endif // A320__0x3B_H
