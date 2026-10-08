/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/* Simplification P4: setting descriptors are const and flash-resident
 * (included via zephyr_linker_sources(ROM_SECTIONS ...)); their mutable
 * state lives in the macro-emitted struct zmk_custom_setting_state blocks
 * in ordinary RAM. The keyspace section stays in RAM - see
 * zmk-custom-settings.ld. */

#include <zephyr/linker/linker-defs.h>

ITERABLE_SECTION_ROM(zmk_custom_setting, 4)
