/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A topology decision is shared by both halves, not independently inferred
 * from the order in which Bluetooth bonds were created. */
enum keypoint_topology {
    KEYPOINT_TOPOLOGY_DONGLE,
    KEYPOINT_TOPOLOGY_LEFT_MASTER,
};

struct keypoint_dual_mode_policy {
    enum keypoint_topology topology;
    bool manual_switch_enabled;
    bool manual_override;
    bool dongle_present;
    int64_t dongle_last_seen_ms;
    uint32_t discovery_grace_ms;
};

void keypoint_dual_mode_init(struct keypoint_dual_mode_policy *policy, int64_t now_ms,
                            uint32_t discovery_grace_ms);
void keypoint_dual_mode_dongle_seen(struct keypoint_dual_mode_policy *policy, int64_t now_ms);
bool keypoint_dual_mode_poll(struct keypoint_dual_mode_policy *policy, int64_t now_ms);
bool keypoint_dual_mode_toggle(struct keypoint_dual_mode_policy *policy);
void keypoint_dual_mode_set_switch_enabled(struct keypoint_dual_mode_policy *policy, bool enabled);
enum keypoint_topology keypoint_dual_mode_right_target(const struct keypoint_dual_mode_policy *policy);
