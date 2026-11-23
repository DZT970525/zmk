/* SPDX-License-Identifier: MIT */
#include "topology_control_client.h"
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/bluetooth/hci.h>
#include <zmk/split/role.h>
#include <zmk/split/peer_selection.h>
/* Keep RIGHT's old link until LEFT actually disconnects following its reboot.
 * A successful confirm write is only queue acceptance, not durable boot-role
 * persistence. If flash fails, LEFT remains connected and confirm is retried. */
enum phase { IDLE, READING, CLAIMING, STAGING, CONFIRMING, WAIT_LEFT, RELEASING };
static enum phase phase;
static struct bt_conn *left_link, *right_link;
static bool left_disconnected, right_staged, completed;
static int result;
static struct keypoint_topology_status status;
static uint16_t nonce;
K_MUTEX_DEFINE(coordinator_lock);
static void poll_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(coordinator_poll,poll_handler);
static void topology_done(int ret,struct keypoint_topology_status value,void *context) {
    ARG_UNUSED(context);k_mutex_lock(&coordinator_lock,K_FOREVER);
    result=ret;status=value;completed=true;
    k_work_reschedule(&coordinator_poll,K_MSEC(25));k_mutex_unlock(&coordinator_lock);
}
static void peer_done(int ret,void *context) {
    topology_done(ret,(struct keypoint_topology_status){0},context);
}
static void reset(void) {
    if (left_link) bt_conn_unref(left_link);
    if (right_link) bt_conn_unref(right_link);
    left_link=right_link=NULL;left_disconnected=right_staged=completed=false;phase=IDLE;
}
static int left_command(uint8_t opcode) {
    completed=false;
    return zmk_keypoint_topology_remote(left_link,opcode,true,topology_done,NULL);
}
static void poll_handler(struct k_work *work) {
    ARG_UNUSED(work);k_mutex_lock(&coordinator_lock,K_FOREVER);
    if (phase==IDLE) {
        if (!zmk_keypoint_config_rpc_ready()) goto done;
        left_link=zmk_keypoint_input_peer_connection(1);
        if (!left_link) goto done;
        phase=READING;
        if (left_command(0)) reset();
        goto done;
    }
    if (phase==WAIT_LEFT) {
        if (left_disconnected) {
            completed=false;phase=RELEASING;
            int ret=zmk_keypoint_peer_release_remote(right_link,ZMK_KEYPOINT_PEER_LEFT,
                nonce,peer_done,NULL);
            if (ret) {phase=WAIT_LEFT;}
        } else {
            phase=CONFIRMING;
            if (left_command(3)) phase=WAIT_LEFT;
        }
        goto done;
    }
    if (!completed) goto done;
    completed=false;
    if (phase==RELEASING) {
        if (!result || result==-ENOTCONN) reset();
        else phase=WAIT_LEFT;
        goto done;
    }
    if (phase==CONFIRMING) {
        /* Even a lost write ACK is resolved by the actual LEFT disconnect. */
        phase=WAIT_LEFT;goto done;
    }
    if (result) {
        if (phase==STAGING) {
            /* Keep claimed LEFT and identical nonce after uncertain ACK. */
            completed=false;
            int ret=zmk_keypoint_peer_stage_remote(right_link,ZMK_KEYPOINT_PEER_LEFT,
                nonce,peer_done,NULL);
            if (ret) {result=ret;completed=true;}
        } else reset();
        goto done;
    }
    if (phase==READING) {
        if (!status.manual || !status.prepared || status.target!=1) {reset();goto done;}
        right_link=zmk_keypoint_input_peer_connection(0);
        if (!right_link) {reset();goto done;}
        if (++nonce==0) ++nonce;
        phase=CLAIMING;
        if (left_command(5)) reset();
    } else if (phase==CLAIMING) {
        if (!status.manual || !status.prepared || status.target!=1) {reset();goto done;}
        phase=STAGING;
        int ret=zmk_keypoint_peer_stage_remote(right_link,ZMK_KEYPOINT_PEER_LEFT,
            nonce,peer_done,NULL);
        if (ret) {result=ret;completed=true;}
    } else if (phase==STAGING) {
        right_staged=true;phase=CONFIRMING;
        if (left_command(3)) phase=WAIT_LEFT;
    }
done:
    k_work_reschedule(&coordinator_poll,K_MSEC(1000));
    k_mutex_unlock(&coordinator_lock);
}
static void disconnected(struct bt_conn *conn,uint8_t reason) {
    ARG_UNUSED(reason);k_mutex_lock(&coordinator_lock,K_FOREVER);
    if (conn==left_link) {
        left_disconnected=true;
        /* Prior to right ACK no topology commit is known. Keep a staged
         * transaction for retry; never release RIGHT based on mere presence. */
        if (!right_staged && phase!=STAGING) reset();
    }
    k_mutex_unlock(&coordinator_lock);
}
static struct bt_conn_cb callbacks={.disconnected=disconnected};
static int coordinator_init(void) {
    bt_conn_cb_register(&callbacks);
    k_work_reschedule(&coordinator_poll,K_MSEC(1000));return 0;
}
SYS_INIT(coordinator_init,APPLICATION,CONFIG_ZMK_BLE_INIT_PRIORITY);
