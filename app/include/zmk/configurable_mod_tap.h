/* SPDX-License-Identifier: MIT
 * Wire representation for the per-binding configurable Mod-Tap.
 *
 * A ZMK binding has two 32-bit parameters. Keep param2 as the unmodified tap
 * keycode and pack the hold keycode plus timeout into param1:
 * bits 0..11 HID usage, bit 12 keyboard/consumer page, bits 13..20 implicit
 * modifier mask, and bits 21..31 tapping term in milliseconds.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define ZMK_CONFIGURABLE_MT_MIN_MS 50u
#define ZMK_CONFIGURABLE_MT_MAX_MS 2000u
#define ZMK_CONFIGURABLE_MT_DEFAULT_MS 400u

static inline uint32_t zmk_configurable_mt_term(uint32_t packed) { return packed >> 21; }

static inline bool zmk_configurable_mt_term_valid(uint32_t packed) {
    uint32_t term = zmk_configurable_mt_term(packed);
    return term >= ZMK_CONFIGURABLE_MT_MIN_MS && term <= ZMK_CONFIGURABLE_MT_MAX_MS;
}

static inline uint32_t zmk_configurable_mt_hold(uint32_t packed) {
    uint32_t page = (packed & (1u << 12)) ? 0x0cu : 0x07u;
    uint32_t modifiers = (packed >> 13) & 0xffu;
    return (modifiers << 24) | (page << 16) | (packed & 0xfffu);
}
