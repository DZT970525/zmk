/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/* RAM (DATA_SECTIONS) half of this module's linker includes: only the
 * keyspace registry stays here. Setting descriptors moved to rodata in
 * simplification P4 - see zmk-custom-settings-rom.ld. */

#include <zephyr/linker/linker-defs.h>

ITERABLE_SECTION_RAM(zmk_custom_setting_keyspace, 4)
