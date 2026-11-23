/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <zmk/split/role.h>
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_CLIENT)
#include <zmk/split/dongle_presence.h>
#endif
#include <zmk/split/peer_selection.h>
#include <zmk/split/bluetooth/peripheral.h>
LOG_MODULE_REGISTER(keypoint_topology_fallback,CONFIG_ZMK_LOG_LEVEL);

static atomic_t started;
static int64_t disconnected_since;
static bool refresh_pending;
#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE_LEFT) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE_LEFT))
static bool requested;
static atomic_t return_phase;
static int return_result;
static uint16_t return_nonce;

static void right_switched(int result,void *context) {
    ARG_UNUSED(context);
    return_result=result;
    atomic_set(&return_phase,2);
}

static void return_to_dongle(void) {
    bool target_central;
    if (zmk_split_role_transition_target(&target_central) || target_central) {
        /* A role request may have been cancelled before right handoff. */
        if (!atomic_get(&return_phase)) { return_result=0; }
        return;
    }
    int phase=atomic_get(&return_phase);
    if (phase==1) { return; }
    if (phase==2) {
        if (return_result) {
            LOG_WRN("Right handoff failed (%d); left remains master",return_result);
            /* A disconnect or lost write ACK does not prove the right kept
             * its old target. Keep ownership and retry if it reconnects.
             * Missing service/invalid request cannot have committed a target. */
            if (return_result==-ENOTSUP || return_result==-EINVAL ||
                return_result==-EACCES) { zmk_split_role_abort_handoff(); }
            atomic_clear(&return_phase);
            return;
        }
        /* The right ACK is sent only after its selected master is durable.
         * Retain it across boot-role persistence retries: the right may have
         * already disconnected, so rediscovering it would lose this handoff. */
        int ret=zmk_split_role_confirm_transition();
        if (ret) { LOG_WRN("Return role confirmation deferred (%d)",ret); }
        return;
    }
    struct bt_conn *right=zmk_keypoint_input_peer_connection(0);
    if (!right) { return; }
    if (zmk_split_role_begin_handoff()) { bt_conn_unref(right); return; }
    /* One return transaction per boot. Preserve its nonce after a lost ACK;
     * the right accepts only identical retries while the old link is held. */
    if (!return_nonce) { return_nonce=1; }
    atomic_set(&return_phase,1);
    int ret=zmk_keypoint_peer_switch_remote(right,ZMK_KEYPOINT_PEER_DONGLE,
                                            return_nonce,right_switched,NULL);
    bt_conn_unref(right);
    if (ret) { zmk_split_role_abort_handoff(); atomic_clear(&return_phase); }
}
#endif
static void poll_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(poll_work,poll_handler);

static void poll_handler(struct k_work *work) {
    ARG_UNUSED(work);
    int64_t now=k_uptime_get();
#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE_LEFT) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE_LEFT))
    if (zmk_split_role_is_central()) {
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_CLIENT)
        if (!zmk_split_role_standalone_selected() && zmk_keypoint_presence_dongle_available() &&
            !zmk_split_role_transition_ready()) { zmk_split_role_request(false); }
#endif
        return_to_dongle();
        goto done;
    }
#endif
    /* An active connection must finish authentication/synchronization rather
     * than be torn down by a presence timeout. Reconnection resets the grace. */
    bool connected=zmk_split_bt_peripheral_is_connected();
    if (!zmk_keypoint_config_ready() || connected) {
#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE_LEFT) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE_LEFT))
        if (connected && requested && zmk_split_role_transition_ready() &&
            !zmk_split_role_cancel_transition()) { requested=false; }
#endif
        disconnected_since=now;
        goto done;
    }
    if (refresh_pending) {
        int ret=zmk_split_bt_peripheral_refresh_advertising();
        if (!ret) { refresh_pending=false; }
        goto done;
    }
    if (now<disconnected_since ||
        now-disconnected_since<CONFIG_ZMK_KEYPOINT_TOPOLOGY_GRACE_MS) { goto done; }
#if (IS_ENABLED(CONFIG_BOARD_KEYPOINT_DONGLE_LEFT) || IS_ENABLED(CONFIG_BOARD_SOFLE_DONGLE_LEFT))
    if (zmk_split_role_is_central()) { goto done; }
    bool target_central=false;
    if (zmk_split_role_transition_ready() && !zmk_split_role_transition_target(&target_central) && target_central) {
        int ret=zmk_split_role_confirm_transition();
        if (ret) { LOG_WRN("Offline role confirmation deferred (%d)",ret); }
    } else {
        /* Preparation serializes and durably checkpoints configuration. A
         * failed prepare clears the role request and can be retried here. */
        int ret=zmk_split_role_request(true);
        if (!ret) { requested=true; }
        else if (ret!=-EBUSY && ret!=-EAGAIN) {
            LOG_WRN("Offline role preparation failed (%d)",ret);
        }
    }
#else
    if (zmk_keypoint_peer_target()==ZMK_KEYPOINT_PEER_DONGLE) {
        int ret=zmk_keypoint_peer_set_target(ZMK_KEYPOINT_PEER_LEFT);
        if (!ret) {
            /* Bond records for both masters remain intact. No active
             * connection exists; replace the old directed advertisement. */
            refresh_pending=true;
        } else { LOG_WRN("Offline right target change failed (%d)",ret); }
    }
#endif
done:
    k_work_reschedule(&poll_work,K_MSEC(250));
}

void zmk_keypoint_topology_start(void) {
    if (!atomic_cas(&started,0,1)) { return; }
    disconnected_since=k_uptime_get();
    k_work_reschedule(&poll_work,K_MSEC(250));
}
