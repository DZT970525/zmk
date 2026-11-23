/* SPDX-License-Identifier: MIT */
#include "dual_mode_policy.h"

void keypoint_dual_mode_init(struct keypoint_dual_mode_policy *policy, int64_t now_ms,
                            uint32_t discovery_grace_ms) {
    *policy = (struct keypoint_dual_mode_policy){
        .topology = KEYPOINT_TOPOLOGY_DONGLE,
        .dongle_last_seen_ms = now_ms,
        .discovery_grace_ms = discovery_grace_ms,
    };
}

void keypoint_dual_mode_dongle_seen(struct keypoint_dual_mode_policy *policy, int64_t now_ms) {
    policy->dongle_present = true;
    policy->dongle_last_seen_ms = now_ms;
}

bool keypoint_dual_mode_poll(struct keypoint_dual_mode_policy *policy, int64_t now_ms) {
    /* Only an authenticated dongle observation may refresh this timestamp.
     * A nearby device with the same advertised name is not sufficient. */
    if (now_ms >= policy->dongle_last_seen_ms &&
        now_ms - policy->dongle_last_seen_ms >= policy->discovery_grace_ms) {
        policy->dongle_present = false;
    }
    if (policy->manual_override) {
        return false;
    }
    const enum keypoint_topology target =
        policy->dongle_present || now_ms - policy->dongle_last_seen_ms < policy->discovery_grace_ms
            ? KEYPOINT_TOPOLOGY_DONGLE
            : KEYPOINT_TOPOLOGY_LEFT_MASTER;
    if (target == policy->topology) {
        return false;
    }
    policy->topology = target;
    return true;
}

bool keypoint_dual_mode_toggle(struct keypoint_dual_mode_policy *policy) {
    if (!policy->manual_switch_enabled) {
        return false;
    }
    policy->manual_override = true;
    policy->topology = policy->topology == KEYPOINT_TOPOLOGY_DONGLE
                           ? KEYPOINT_TOPOLOGY_LEFT_MASTER
                           : KEYPOINT_TOPOLOGY_DONGLE;
    return true;
}

void keypoint_dual_mode_set_switch_enabled(struct keypoint_dual_mode_policy *policy, bool enabled) {
    policy->manual_switch_enabled = enabled;
    if (!enabled) {
        policy->manual_override = false;
    }
}

enum keypoint_topology keypoint_dual_mode_right_target(const struct keypoint_dual_mode_policy *policy) {
    return policy->topology;
}
