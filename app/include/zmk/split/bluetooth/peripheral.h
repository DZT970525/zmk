/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

bool zmk_split_bt_peripheral_is_connected(void);

bool zmk_split_bt_peripheral_is_bonded(void);

/* After an offline target change, replace the old directed advertisement.
 * Never disconnect an active peer merely to refresh advertising. */
int zmk_split_bt_peripheral_refresh_advertising(void);
