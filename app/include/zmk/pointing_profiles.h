/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

/* Optional board-owned profiles. Other boards retain their existing settings. */
void zmk_pointing_profile_begin(void) __attribute__((weak));
void zmk_pointing_profile_end(void) __attribute__((weak));
void zmk_pointing_profiles_start(void) __attribute__((weak));
uint32_t zmk_pointing_profile_epoch(void) __attribute__((weak));
