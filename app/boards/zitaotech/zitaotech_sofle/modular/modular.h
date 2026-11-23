/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stdbool.h>
enum sofle_module_kind { SOFLE_MODULE_NONE, SOFLE_MODULE_TOUCHPAD, SOFLE_MODULE_TRACKPOINT };
uint8_t sofle_modular_local_kind(void);
uint32_t sofle_modular_scan_column(uint32_t row, uint32_t column);
void sofle_modular_right_arrow(void);
int sofle_modular_step_dpi(bool right, int delta);
