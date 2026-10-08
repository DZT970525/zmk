/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

/*
 * Raised on the central whenever the split transport's connection status
 * changes, i.e. when a peripheral half comes or goes.  Features that need to
 * mirror "is the other half there?" into Studio use it instead of polling.
 */
struct zmk_split_central_status_changed {
    bool connected;
};

ZMK_EVENT_DECLARE(zmk_split_central_status_changed);
