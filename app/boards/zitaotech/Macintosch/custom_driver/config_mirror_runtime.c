/* SPDX-License-Identifier: MIT */
#include "config_snapshot.h"
#include "config_export_client.h"
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zmk/split/role.h>
#include <zmk/split/peer_selection.h>
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_SERVER)
#include <zmk/split/dongle_presence.h>
#endif
LOG_MODULE_REGISTER(keypoint_config_mirror, CONFIG_ZMK_LOG_LEVEL);

/* The left assigns revisions. The dongle stores a recovery mirror and never
 * checkpoints its defaults as a competing authority. One buffer is shared by
 * downloads and Studio proposals, serialized by config_mutex. */
static uint8_t staging[KEYPOINT_CONFIG_MAX_SIZE];
static struct keypoint_config_store store;
static struct bt_conn *synced_left;
static struct k_work_q config_queue;
K_THREAD_STACK_DEFINE(config_stack,5120);
K_MUTEX_DEFINE(config_mutex);
K_SEM_DEFINE(transfer_done,0,1);
K_SEM_DEFINE(checkpoint_done,0,1);
static atomic_t started, ready, synchronized;
static bool dirty, boot_recovered, boot_live_valid;
static uint32_t boot_live_crc;
static size_t boot_live_length;
static uint32_t nonce;
static int transfer_result, checkpoint_result;
static size_t transfer_length;
static struct keypoint_config_revision transfer_revision;

bool zmk_keypoint_config_ready(void) { return atomic_get(&ready)!=0; }
bool zmk_keypoint_config_rpc_ready(void) {
    return zmk_keypoint_config_ready() && atomic_get(&synchronized);
}
void zmk_keypoint_config_lock(void) { k_mutex_lock(&config_mutex,K_FOREVER); }
void zmk_keypoint_config_unlock(void) { k_mutex_unlock(&config_mutex); }
int zmk_keypoint_config_prepare_transition(void) { return -ENOTSUP; }
void zmk_keypoint_config_cancel_transition(void) {}

static uint32_t next_nonce(void) { if (++nonce==0) { ++nonce; } return nonce; }
static void transferred(int result,struct keypoint_config_revision revision,
                        size_t length,void *context) {
    ARG_UNUSED(context);
    transfer_result=result; transfer_revision=revision; transfer_length=length;
    k_sem_give(&transfer_done);
}
static int wait_transfer(int start_result) {
    if (start_result) { return start_result; }
    /* The transfer client owns its timeout and retains the borrowed staging
     * buffer until its completion callback, including disconnect/error paths. */
    int ret=k_sem_take(&transfer_done,K_FOREVER);
    return ret ? ret : transfer_result;
}
static void forget_left(void) {
    if (synced_left) { bt_conn_unref(synced_left); synced_left=NULL; }
    atomic_clear(&synchronized);
}

static void poll_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(poll_work,poll_handler);
static void poll_handler(struct k_work *work) {
    ARG_UNUSED(work);
    /* Never wait on an RPC-owned mutex ahead of its queued checkpoint. */
    if (k_mutex_lock(&config_mutex,K_NO_WAIT)) {
        k_work_reschedule_for_queue(&config_queue,&poll_work,K_MSEC(100)); return;
    }
    struct bt_conn *left=zmk_keypoint_input_peer_connection(1);
    if (!left) {
        forget_left();
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_SERVER)
        zmk_keypoint_presence_update_left(NULL);
#endif
        goto done;
    }
    if (left==synced_left && atomic_get(&synchronized)) {
        bt_conn_unref(left); goto done;
    }
    forget_left();
    k_sem_reset(&transfer_done);
    int ret=wait_transfer(zmk_keypoint_config_download(left,next_nonce(),staging,
        sizeof(staging),transferred,NULL));
    if (!ret) { ret=keypoint_config_snapshot_validate(staging,transfer_length); }
    bool changed=!ret && (store.committed.generation!=transfer_revision.generation ||
                           store.committed.crc!=transfer_revision.crc);
    if (!ret && changed) {
        ret=keypoint_config_snapshot_commit(staging,transfer_length,transfer_revision,&store);
    }
    bool matches_live=!ret && boot_live_valid && boot_live_length==transfer_length &&
                       boot_live_crc==transfer_revision.crc;
    if (!ret && zmk_keypoint_config_ready() && (dirty || (changed && !matches_live))) {
        /* Reconnect may carry a new keymap. Replay at reboot rather than
         * replace bindings underneath held keys or an executing macro. */
        sys_reboot(SYS_REBOOT_COLD);
        ret=-EAGAIN; /* If reboot returns, do not report live application. */
    } else if (!ret && !zmk_keypoint_config_ready()) {
        if ((!boot_recovered || changed) &&
            !(boot_live_valid && boot_live_length==transfer_length && boot_live_crc==transfer_revision.crc)) {
            ret=keypoint_config_snapshot_restore(&store,staging,transfer_length);
        }
        if (!ret) { atomic_set(&ready,1); }
    }
    if (!ret) {
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_DONGLE_PRESENCE_SERVER)
        ret=zmk_keypoint_presence_update_left(left);
#endif
    }
    if (!ret) {
        synced_left=bt_conn_ref(left); atomic_set(&synchronized,1);
        boot_live_valid=false;
    } else { LOG_WRN("Left configuration synchronization failed (%d)",ret); }
    bt_conn_unref(left);
done:
    k_mutex_unlock(&config_mutex);
    k_work_reschedule_for_queue(&config_queue,&poll_work,K_MSEC(1000));
}

static void checkpoint_handler(struct k_work *work) {
    ARG_UNUSED(work);
    /* The RPC thread owns config_mutex until this work and its transfer finish. */
    size_t length=0;
    dirty=true;
    int ret=zmk_keypoint_config_rpc_ready() ?
        keypoint_config_snapshot_capture(staging,sizeof(staging),&length) : -EAGAIN;
    if (!ret && keypoint_config_crc32(staging,length)==store.committed.crc) { dirty=false; }
    if (!ret && dirty) {
        ret=keypoint_config_snapshot_validate(staging,length);
        struct bt_conn *left=zmk_keypoint_input_peer_connection(1);
        if (!ret && (!left || left!=synced_left)) { ret=-ENOTCONN; }
        if (!ret) {
            k_sem_reset(&transfer_done);
            ret=wait_transfer(zmk_keypoint_config_propose(left,next_nonce(),store.committed,
                staging,length,transferred,NULL));
        }
        if (left) { bt_conn_unref(left); }
        if (!ret) {
            ret=keypoint_config_snapshot_commit(staging,length,transfer_revision,&store);
        }
        if (!ret) { dirty=false; }
    }
    if (ret) { forget_left(); }
    checkpoint_result=ret; k_sem_give(&checkpoint_done);
}
K_WORK_DEFINE(checkpoint_work,checkpoint_handler);
int zmk_keypoint_config_checkpoint_after_rpc(void) {
    k_sem_reset(&checkpoint_done);
    int ret=k_work_submit_to_queue(&config_queue,&checkpoint_work);
    if (ret<0) { forget_left(); return ret; }
    ret=k_sem_take(&checkpoint_done,K_FOREVER);
    return ret ? ret : checkpoint_result;
}

static void boot_handler(struct k_work *work) {
    ARG_UNUSED(work);
    zmk_keypoint_config_lock();
    keypoint_config_settings_init(&store,1);
    size_t length;
    int ret=keypoint_config_store_load(&store,staging,sizeof(staging),&length);
    if (!ret) { ret=keypoint_config_snapshot_restore(&store,staging,length); }
    boot_recovered=!ret;
    /* First boot commonly matches the compiled keymap. Compare it before the
     * transfer overwrites staging, avoiding a full legacy replay and flash
     * rewrite when the left authority confirms those exact bytes. */
    int live_result=keypoint_config_snapshot_capture(staging,sizeof(staging),&boot_live_length);
    if (!live_result) live_result=keypoint_config_snapshot_validate(staging,boot_live_length);
    boot_live_valid=!live_result;
    if (boot_live_valid) boot_live_crc=keypoint_config_crc32(staging,boot_live_length);
    if (ret && ret!=-ENOENT) { LOG_WRN("Configuration mirror recovery failed (%d)",ret); }
    /* Input can use a validated recovered mirror, or the compiled keymap on a
     * clean first boot. Authority synchronization gates Studio edits separately:
     * an unavailable transfer must not disable every key, including recovery.
     * Defaults are never committed as a competing authoritative revision. A
     * different left snapshot is committed and replayed at reboot. */
    if (boot_live_valid && (!ret || ret==-ENOENT)) atomic_set(&ready,1);
    zmk_keypoint_config_unlock();
    k_work_reschedule_for_queue(&config_queue,&poll_work,K_NO_WAIT);
}
K_WORK_DEFINE(boot_work,boot_handler);
void zmk_keypoint_config_start(void) {
    if (!atomic_cas(&started,0,1)) { return; }
    k_work_queue_start(&config_queue,config_stack,K_THREAD_STACK_SIZEOF(config_stack),
                       K_PRIO_PREEMPT(5),NULL);
    k_work_submit_to_queue(&config_queue,&boot_work);
}
