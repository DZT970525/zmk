/* SPDX-License-Identifier: MIT */
#pragma once
#include <zephyr/bluetooth/conn.h>
struct keypoint_topology_status {
    bool enabled, central, prepared, manual;
    uint8_t target;
};
typedef void (*keypoint_topology_callback)(int result,
    struct keypoint_topology_status status, void *context);
/* opcode 0 reads; 1 persists permission; 2 prepares; 3 confirms; 4 cancels; 5 claims handoff ownership.
 * Writes are followed by a read of authoritative status, except confirm: its
 * successful write ACK precedes reboot, and returned status is zeroed. */
int zmk_keypoint_topology_remote(struct bt_conn *left, uint8_t opcode, bool value,
    keypoint_topology_callback callback, void *context);
