/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

/* Keep values stable: used in custom settings persistence (0 doubles as
 * "no override"/AUTO) and mapped one-to-one to the RPC proto's Os enum.
 * Append new values at the end - never renumber existing ones. */
enum zmk_os {
    ZMK_OS_UNKNOWN = 0,
    ZMK_OS_WINDOWS = 1,
    ZMK_OS_MACOS = 2,
    ZMK_OS_LINUX = 3,
    ZMK_OS_IOS = 4,
    ZMK_OS_ANDROID = 5,
};

/* OS for the currently active (USB or BLE) endpoint, override applied. */
enum zmk_os zmk_os_detection_current(void);

struct zmk_os_changed {
    enum zmk_os os;
};

ZMK_EVENT_DECLARE(zmk_os_changed);
