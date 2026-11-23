/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#if defined(CONFIG_ZMK_KEYPOINT_TOPOLOGY_FALLBACK)
void zmk_keypoint_topology_start(void);
#endif

#if defined(CONFIG_ZMK_KEYPOINT_CONFIG_SYNC) || defined(CONFIG_ZMK_KEYPOINT_CONFIG_MIRROR)
bool zmk_keypoint_config_ready(void);
bool zmk_keypoint_config_rpc_ready(void);
void zmk_keypoint_config_start(void);
void zmk_keypoint_config_lock(void);
void zmk_keypoint_config_unlock(void);
int zmk_keypoint_config_checkpoint_after_rpc(void);
int zmk_keypoint_config_prepare_transition(void);
void zmk_keypoint_config_cancel_transition(void);
#else
static inline bool zmk_keypoint_config_ready(void) { return true; }
static inline bool zmk_keypoint_config_rpc_ready(void) { return true; }
static inline void zmk_keypoint_config_lock(void) {}
static inline void zmk_keypoint_config_unlock(void) {}
static inline int zmk_keypoint_config_checkpoint_after_rpc(void) { return 0; }
static inline int zmk_keypoint_config_prepare_transition(void) { return 0; }
static inline void zmk_keypoint_config_cancel_transition(void) {}
#endif

/* Hybrid transports use this predicate instead of interpreting the build-time
 * CENTRAL flag as the current topology. Ordinary boards retain their existing
 * constant role. */
#if defined(CONFIG_ZMK_SPLIT_RUNTIME_ROLE)
bool zmk_split_role_is_central(void);
bool zmk_split_role_manual_switch_enabled(void);
bool zmk_split_role_standalone_selected(void);
int zmk_split_role_set_manual_switch_enabled(bool enabled);
int zmk_split_role_request(bool central);
bool zmk_split_role_transition_ready(void);
bool zmk_split_role_transition_is_manual(void);
int zmk_split_role_transition_target(bool *central);
int zmk_split_role_begin_handoff(void);
int zmk_split_role_abort_handoff(void);
int zmk_split_role_confirm_transition(void);
int zmk_split_role_cancel_transition(void);
int zmk_split_role_toggle(void);
#else
static inline bool zmk_split_role_is_central(void) {
#if defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !defined(CONFIG_ZMK_SPLIT)
    return true;
#else
    return false;
#endif
}
#endif
