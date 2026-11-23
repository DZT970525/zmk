/* SPDX-License-Identifier: MIT */
#include "config_snapshot.h"
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <zmk/split/role.h>
#include "config_export.h"
#include <string.h>
LOG_MODULE_REGISTER(keypoint_config_sync, CONFIG_ZMK_LOG_LEVEL);

/* One staging buffer serves load/capture/replay. No second full snapshot or
 * flash writes from an RPC/Bluetooth callback are required. */
static uint8_t staging[KEYPOINT_CONFIG_MAX_SIZE];
static struct keypoint_config_store store;
static atomic_t ready;
static atomic_t started;
static atomic_t exporting;
static atomic_t replay_required;
static atomic_t transition_frozen;
static struct k_work_q config_queue;
K_MUTEX_DEFINE(config_mutex);
K_THREAD_STACK_DEFINE(config_stack, 5120);
K_SEM_DEFINE(checkpoint_done,0,1);
static int checkpoint_result;
static void checkpoint_work_handler(struct k_work *work);
K_WORK_DEFINE(checkpoint_work,checkpoint_work_handler);

bool zmk_keypoint_config_ready(void) { return atomic_get(&ready)!=0; }
bool zmk_keypoint_config_rpc_ready(void) {
    return zmk_keypoint_config_ready() && !atomic_get(&exporting) &&
           !atomic_get(&replay_required) && !atomic_get(&transition_frozen);
}
void zmk_keypoint_config_lock(void) { k_mutex_lock(&config_mutex,K_FOREVER); }
void zmk_keypoint_config_unlock(void) { k_mutex_unlock(&config_mutex); }

static int checkpoint(void) {
    /* The RPC dispatcher holds config_mutex until this finishes. Legacy
     * writes and the authoritative snapshot become durable before the client
     * sees success, rather than leaving a delayed-save crash window. */
    if (!zmk_keypoint_config_ready() || atomic_get(&replay_required)) { return -EAGAIN; }
    size_t length;
    int ret=keypoint_config_snapshot_capture(staging,sizeof(staging),&length);
    if (ret) { return ret; }
    if (keypoint_config_crc32(staging,length)==store.committed.crc) { return 0; }
    return keypoint_config_snapshot_checkpoint(&store,staging,sizeof(staging),&length);
}

static void checkpoint_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    /* The waiting RPC caller owns config_mutex. No other queue handler may
     * block on that mutex ahead of this work; future network jobs must use
     * nonblocking acquisition/rescheduling instead. */
    checkpoint_result=checkpoint();
    k_sem_give(&checkpoint_done);
}

int zmk_keypoint_config_checkpoint_after_rpc(void) {
    if (!zmk_keypoint_config_ready()) { return -EAGAIN; }
    k_sem_reset(&checkpoint_done);
    int ret=k_work_submit_to_queue(&config_queue,&checkpoint_work);
    if (ret<0) { return ret; }
    ret=k_sem_take(&checkpoint_done,K_FOREVER);
    return ret<0 ? ret : checkpoint_result;
}

int zmk_keypoint_config_prepare_transition(void) {
    if (k_mutex_lock(&config_mutex,K_NO_WAIT)) { return -EBUSY; }
    int ret=0;
    if (!zmk_keypoint_config_ready()) { ret=-EAGAIN; }
    else if (atomic_get(&exporting)) { ret=-EBUSY; }
    else if (!atomic_get(&transition_frozen)) {
        /* A received proposal is already durable. Capturing the old live
         * keymap here would overwrite it. Otherwise flush on the config queue
         * before promising that it is safe to persist a new boot role. */
        if (!atomic_get(&replay_required)) {
            ret=zmk_keypoint_config_checkpoint_after_rpc();
        }
        if (!ret) { atomic_set(&transition_frozen,1); }
    }
    k_mutex_unlock(&config_mutex);
    return ret;
}

void zmk_keypoint_config_cancel_transition(void) {
    zmk_keypoint_config_lock();
    atomic_clear(&transition_frozen);
    zmk_keypoint_config_unlock();
}

static int recover(void) {
    keypoint_config_settings_init(&store,1);
    size_t length;
    int ret=keypoint_config_store_load(&store,staging,sizeof(staging),&length);
    if (ret==-ENOENT) {
        /* First boot: all legacy settings/defaults have finished loading. */
        return keypoint_config_snapshot_checkpoint(&store,staging,sizeof(staging),&length);
    }
    if (ret) { return ret; }
    /* Avoid rewriting every legacy setting on an already consistent boot.
     * Capture reuses staging; reload the journal only if replay is needed. */
    size_t current_length;
    ret=keypoint_config_snapshot_capture(staging,sizeof(staging),&current_length);
    if (!ret && current_length==length &&
        keypoint_config_crc32(staging,current_length)==store.committed.crc) {
        return keypoint_config_snapshot_validate(staging,current_length);
    }
    ret=keypoint_config_store_load(&store,staging,sizeof(staging),&length);
    if (ret) { return ret; }
    return keypoint_config_snapshot_restore(&store,staging,length);
}
static void recover_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    zmk_keypoint_config_lock();
    int ret=recover();
    if (ret) {
        LOG_ERR("Configuration replay failed (%d); input remains gated",ret);
        zmk_keypoint_config_unlock();
        return;
    }
    atomic_clear(&replay_required);
    atomic_set(&ready,1);
    zmk_keypoint_config_unlock();
    LOG_INF("Authoritative configuration ready");
}
K_WORK_DEFINE(recover_work,recover_work_handler);

#if defined(CONFIG_ZMK_KEYPOINT_CONFIG_TRANSFER)
static struct keypoint_config_export_status export_status;
static struct keypoint_config_transaction proposal;
static struct keypoint_config_revision proposal_base;
static int64_t export_deadline;
static void export_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(export_work,export_work_handler);
static void export_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    /* Never wait behind an RPC that is waiting for checkpoint_work on this
     * same queue. The caller can retry a busy export. */
    int ret=k_mutex_lock(&config_mutex,K_NO_WAIT);
    if (ret) {
        k_work_reschedule_for_queue(&config_queue,&export_work,K_MSEC(10));
        return;
    }
    if (export_status.state==KEYPOINT_PROPOSAL_SAVE_QUEUED) {
        ret=keypoint_config_commit(&proposal,keypoint_config_snapshot_commit,&store);
        export_status.result=ret;
        export_status.revision=store.committed;
        export_status.state=ret ? KEYPOINT_EXPORT_FAILED : KEYPOINT_PROPOSAL_SAVED;
        if (!ret) { atomic_set(&replay_required,1); }
        k_mutex_unlock(&config_mutex);
        return;
    }
    if (export_status.state!=KEYPOINT_EXPORT_QUEUED) {
        k_mutex_unlock(&config_mutex);
        return;
    }
    /* The journal may be newer than the peripheral's legacy live keymap.
     * Never checkpoint that stale live copy over an accepted dongle proposal. */
    ret=atomic_get(&replay_required) ? 0 : checkpoint();
    if (!ret) {
        ret=keypoint_config_store_load(&store,staging,sizeof(staging),&export_status.length);
    }
    export_status.result=ret;
    export_status.revision=store.committed;
    export_status.state=ret ? KEYPOINT_EXPORT_FAILED : KEYPOINT_EXPORT_AVAILABLE;
    k_mutex_unlock(&config_mutex);
}

static void export_expiry_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(export_expiry,export_expiry_handler);
static void export_expiry_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (k_mutex_lock(&config_mutex,K_NO_WAIT)) {
        k_work_reschedule(&export_expiry,K_MSEC(100));
        return;
    }
    if (atomic_get(&exporting) && (k_uptime_get()<export_deadline ||
                                  export_status.state==KEYPOINT_PROPOSAL_SAVE_QUEUED)) {
        k_work_reschedule(&export_expiry,K_MSEC(1000));
    } else {
        export_status=(struct keypoint_config_export_status){0};
        keypoint_config_abort(&proposal);
        atomic_clear(&exporting);
    }
    k_mutex_unlock(&config_mutex);
}

int zmk_keypoint_config_export_start(uint32_t nonce) {
    if (!nonce) { return -EINVAL; }
    if (k_mutex_lock(&config_mutex,K_NO_WAIT)) { return -EBUSY; }
    int ret=0;
    if (!zmk_keypoint_config_ready()) { ret=-EAGAIN; }
    else if (atomic_get(&transition_frozen)) { ret=-EBUSY; }
    else if (atomic_get(&exporting)) {
        ret=export_status.nonce==nonce &&
            (export_status.state==KEYPOINT_EXPORT_QUEUED ||
             export_status.state==KEYPOINT_EXPORT_AVAILABLE ||
             (export_status.state==KEYPOINT_EXPORT_FAILED && !proposal.active)) ? 0 : -EBUSY;
    } else {
        export_status=(struct keypoint_config_export_status){
            .state=KEYPOINT_EXPORT_QUEUED,.nonce=nonce};
        export_deadline=k_uptime_get()+30000;
        atomic_set(&exporting,1);
        ret=k_work_reschedule_for_queue(&config_queue,&export_work,K_NO_WAIT);
        if (ret<0) {
            export_status=(struct keypoint_config_export_status){0};
            atomic_clear(&exporting);
        } else {
            k_work_reschedule(&export_expiry,K_MSEC(1000));
            ret=0;
        }
    }
    k_mutex_unlock(&config_mutex);
    return ret;
}
int zmk_keypoint_config_export_status(struct keypoint_config_export_status *status) {
    if (!status) { return -EINVAL; }
    if (k_mutex_lock(&config_mutex,K_NO_WAIT)) { return -EBUSY; }
    *status=export_status;
    k_mutex_unlock(&config_mutex);
    return 0;
}
int zmk_keypoint_config_export_read(uint32_t nonce,size_t offset,void *buffer,size_t length) {
    if (!buffer || !length) { return -EINVAL; }
    if (k_mutex_lock(&config_mutex,K_NO_WAIT)) { return -EBUSY; }
    int ret=0;
    if (!atomic_get(&exporting) || export_status.nonce!=nonce) { ret=-ESTALE; }
    else if (export_status.state!=KEYPOINT_EXPORT_AVAILABLE) { ret=-EAGAIN; }
    else if (offset>export_status.length || length>export_status.length-offset) { ret=-EMSGSIZE; }
    else {
        memcpy(buffer,staging+offset,length);
        export_deadline=k_uptime_get()+30000;
    }
    k_mutex_unlock(&config_mutex);
    return ret;
}
int zmk_keypoint_config_export_release(uint32_t nonce) {
    if (k_mutex_lock(&config_mutex,K_NO_WAIT)) { return -EBUSY; }
    int ret=0;
    if (!atomic_get(&exporting) || nonce!=export_status.nonce) { ret=-ESTALE; }
    else if (export_status.state==KEYPOINT_PROPOSAL_SAVE_QUEUED) { ret=-EINPROGRESS; }
    else {
        export_status=(struct keypoint_config_export_status){0};
        keypoint_config_abort(&proposal);
        atomic_clear(&exporting);
    }
    k_mutex_unlock(&config_mutex);
    return ret;
}

int zmk_keypoint_config_proposal_start(uint32_t nonce,struct keypoint_config_revision base) {
    if (!nonce) { return -EINVAL; }
    if (k_mutex_lock(&config_mutex,K_NO_WAIT)) { return -EBUSY; }
    int ret=0;
    if (!zmk_keypoint_config_ready()) { ret=-EAGAIN; }
    else if (atomic_get(&transition_frozen)) { ret=-EBUSY; }
    else if (atomic_get(&exporting)) {
        ret=export_status.nonce==nonce && proposal_base.generation==base.generation &&
            proposal_base.crc==base.crc && export_status.state>=KEYPOINT_PROPOSAL_RECEIVING
            ? 0 : -EBUSY;
    } else if (base.generation!=store.committed.generation || base.crc!=store.committed.crc) {
        ret=-ESTALE;
    } else if (base.generation==UINT64_MAX) { ret=-EOVERFLOW; }
    else {
        proposal=(struct keypoint_config_transaction){.staging=staging,.capacity=sizeof(staging),
            .schema=1,.committed=store.committed};
        proposal_base=base;
        export_status=(struct keypoint_config_export_status){.state=KEYPOINT_PROPOSAL_METADATA,
            .nonce=nonce,.revision=store.committed};
        export_deadline=k_uptime_get()+30000;
        atomic_set(&exporting,1);
        k_work_reschedule(&export_expiry,K_MSEC(1000));
    }
    k_mutex_unlock(&config_mutex);
    return ret;
}
int zmk_keypoint_config_proposal_metadata(uint32_t nonce,uint32_t crc,size_t length) {
    if (k_mutex_lock(&config_mutex,K_NO_WAIT)) { return -EBUSY; }
    int ret=0;
    if (!atomic_get(&exporting) || nonce!=export_status.nonce) { ret=-ESTALE; }
    else if (export_status.state==KEYPOINT_PROPOSAL_RECEIVING ||
             export_status.state==KEYPOINT_PROPOSAL_SAVE_QUEUED ||
             export_status.state==KEYPOINT_PROPOSAL_SAVED) {
        ret=proposal.incoming.crc==crc && proposal.length==length ? 0 : -EBADMSG;
    } else if (export_status.state!=KEYPOINT_PROPOSAL_METADATA) { ret=-EBUSY; }
    else {
        struct keypoint_config_revision incoming={proposal.committed.generation+1,crc};
        ret=keypoint_config_begin(&proposal,1,proposal.committed,incoming,length);
        if (!ret) {
            export_status.state=KEYPOINT_PROPOSAL_RECEIVING;
            export_status.length=length;
            export_deadline=k_uptime_get()+30000;
        }
    }
    k_mutex_unlock(&config_mutex);
    return ret;
}
int zmk_keypoint_config_proposal_chunk(uint32_t nonce,size_t offset,const uint8_t *data,size_t length) {
    if (k_mutex_lock(&config_mutex,K_NO_WAIT)) { return -EBUSY; }
    int ret;
    if (!atomic_get(&exporting) || nonce!=export_status.nonce) { ret=-ESTALE; }
    else if (export_status.state==KEYPOINT_PROPOSAL_SAVE_QUEUED ||
             export_status.state==KEYPOINT_PROPOSAL_SAVED) {
        /* A retried write may arrive after durable acceptance. It may only
         * repeat already received bytes, never change the accepted proposal. */
        ret=!data || !length || offset>proposal.received || length>proposal.received-offset ?
            -EINVAL : memcmp(staging+offset,data,length) ? -EBADMSG : 0;
    }
    else if (export_status.state!=KEYPOINT_PROPOSAL_RECEIVING) { ret=-EBUSY; }
    else {
        ret=keypoint_config_chunk(&proposal,offset,data,length);
        if (!ret) { export_deadline=k_uptime_get()+30000; }
    }
    k_mutex_unlock(&config_mutex);
    return ret;
}
int zmk_keypoint_config_proposal_save(uint32_t nonce) {
    if (k_mutex_lock(&config_mutex,K_NO_WAIT)) { return -EBUSY; }
    int ret=0;
    if (!atomic_get(&exporting) || nonce!=export_status.nonce) { ret=-ESTALE; }
    else if (export_status.state==KEYPOINT_PROPOSAL_SAVED ||
             export_status.state==KEYPOINT_PROPOSAL_SAVE_QUEUED) { /* Idempotent retry. */ }
    else if ((export_status.state!=KEYPOINT_PROPOSAL_RECEIVING &&
              export_status.state!=KEYPOINT_EXPORT_FAILED) || !proposal.active) { ret=-EBUSY; }
    else if (proposal.received!=proposal.length) { ret=-EAGAIN; }
    else {
        export_status.state=KEYPOINT_PROPOSAL_SAVE_QUEUED;
        ret=k_work_reschedule_for_queue(&config_queue,&export_work,K_NO_WAIT);
        if (ret<0) { export_status.state=KEYPOINT_EXPORT_FAILED; export_status.result=ret; }
        else { ret=0; }
    }
    k_mutex_unlock(&config_mutex);
    return ret;
}
#endif

void zmk_keypoint_config_start(void) {
    /* Called exactly after settings_load returns, not during h_commit. */
    if (!atomic_cas(&started,0,1)) { return; }
    k_work_queue_start(&config_queue,config_stack,K_THREAD_STACK_SIZEOF(config_stack),
                        K_PRIO_PREEMPT(5),NULL);
    int ret=k_work_submit_to_queue(&config_queue,&recover_work);
    if (ret<0) { LOG_ERR("Unable to queue configuration replay (%d)",ret); }
}
