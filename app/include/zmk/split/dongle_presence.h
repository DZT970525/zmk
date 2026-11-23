/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <zephyr/bluetooth/conn.h>

/* Called by the mirror bridge after the encrypted snapshot was verified.
 * A null peer starts offline directed advertising to the stored left identity. */
int zmk_keypoint_presence_update_left(struct bt_conn *left);
bool zmk_keypoint_presence_is_control_connection(struct bt_conn *conn);

/* Left master probe: consume only an advertisement from its stored dongle. */
bool zmk_keypoint_presence_probe_advertisement(const bt_addr_le_t *address);
bool zmk_keypoint_presence_probe_connection(struct bt_conn *conn);
bool zmk_keypoint_presence_dongle_available(void);
int zmk_keypoint_presence_scan_pause(void);
int zmk_keypoint_presence_scan_resume(void);
