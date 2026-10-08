/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum zmk_activity_state { ZMK_ACTIVITY_ACTIVE, ZMK_ACTIVITY_IDLE, ZMK_ACTIVITY_SLEEP };

enum zmk_activity_state zmk_activity_get_state(void);

/*
 * Mark the keyboard as active from outside the event system.
 *
 * ZMK notes activity from key/position events and from input subsystem
 * reports.  A pointing device that does not report through the input subsystem
 * - the trackpad in direction-key mode synthesizes arrow keys directly - would
 * otherwise leave the keyboard idle, so its backlight stayed off while the pad
 * was being used.  Safe to call from any thread; the state change is applied on
 * the system work queue.
 */
void zmk_activity_note(void);

/*
 * Runtime-configurable inactivity timeouts.
 *
 * These back the ZitaoTech Studio "sleep settings" page (the settings RPC
 * module reads and writes them).  They default to the build time
 * CONFIG_ZMK_IDLE_TIMEOUT / CONFIG_ZMK_IDLE_SLEEP_TIMEOUT values and are
 * persisted across reboots once changed.  A value of 0 disables that timeout.
 */
uint32_t zmk_activity_get_idle_ms(void);
uint32_t zmk_activity_get_sleep_ms(void);
bool zmk_activity_set_idle_ms(uint32_t idle_ms);
bool zmk_activity_set_sleep_ms(uint32_t sleep_ms);
