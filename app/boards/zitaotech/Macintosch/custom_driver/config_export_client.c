/* SPDX-License-Identifier: MIT */
#include "config_export_client.h"
#include "config_export.h"
#include "config_sync_journal.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zmk/split/role.h>
#include <zmk/split/peer_selection.h>

#define EXPORT_UUID(n) BT_UUID_128_ENCODE(n,0xea4d,0x4fbe,0xa448,0x7d839ed0bdb0)
static const struct bt_uuid_128 command_uuid=BT_UUID_INIT_128(EXPORT_UUID(0xcf03e408));
static const struct bt_uuid_128 status_uuid=BT_UUID_INIT_128(EXPORT_UUID(0xcf03e409));
static const struct bt_uuid_128 data_uuid=BT_UUID_INIT_128(EXPORT_UUID(0xcf03e40a));
enum download_phase { DOWNLOAD_IDLE, DOWNLOAD_DISCOVER, DOWNLOAD_START,
                      DOWNLOAD_STATUS, DOWNLOAD_DATA, DOWNLOAD_RELEASE,
                      UPLOAD_BASE, UPLOAD_METADATA, UPLOAD_CHUNK, UPLOAD_SAVE };
static struct download_state {
    enum download_phase phase;
    struct bt_conn *conn;
    struct bt_gatt_discover_params discover;
    struct bt_gatt_read_params read;
    struct bt_gatt_write_params write;
    uint16_t command_handle,status_handle,data_handle;
    uint8_t command[244],status[24];
    bool upload;
    struct keypoint_config_revision base;
    uint32_t proposal_crc;
    size_t chunk_length;
    size_t status_received,received,length,capacity;
    uint8_t *buffer;
    struct keypoint_config_revision revision;
    keypoint_config_download_callback callback;
    void *context;
    int result;
} download;
K_MUTEX_DEFINE(download_lock);
static void poll_handler(struct k_work *work);
static void timeout_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(download_poll,poll_handler);
K_WORK_DELAYABLE_DEFINE(download_timeout,timeout_handler);

/* All state transitions hold download_lock. The completion callback runs
 * after releasing it, so the caller may schedule the next transfer. */
static void complete(int result) {
    struct bt_conn *conn=download.conn;
    if (!conn) { return; }
    keypoint_config_download_callback callback=download.callback;
    void *context=download.context;
    struct keypoint_config_revision revision=download.revision;
    size_t length=download.length;
    k_work_cancel_delayable(&download_poll);
    k_work_cancel_delayable(&download_timeout);
    bt_gatt_cancel(conn,&download.discover);
    bt_gatt_cancel(conn,&download.read);
    bt_gatt_cancel(conn,&download.write);
    download.phase=DOWNLOAD_IDLE;
    download.conn=NULL;
    bt_conn_unref(conn);
    k_mutex_unlock(&download_lock);
    if (callback) { callback(result,revision,result ? 0 : length,context); }
    k_mutex_lock(&download_lock,K_FOREVER);
}
static uint8_t read_value(struct bt_conn *conn,uint8_t error,struct bt_gatt_read_params *params,
                           const void *data,uint16_t length);
static void written(struct bt_conn *conn,uint8_t error,struct bt_gatt_write_params *params);
static void release_session(int result) {
    download.result=result;
    download.phase=DOWNLOAD_RELEASE;
    download.command[1]=2;
    download.write=(struct bt_gatt_write_params){.func=written,.handle=download.command_handle,
        .data=download.command,.length=6};
    int ret=bt_gatt_write(download.conn,&download.write);
    if (ret) { complete(result ? result : ret); }
}
static void read_status(void) {
    download.phase=DOWNLOAD_STATUS;
    download.status_received=0;
    download.read=(struct bt_gatt_read_params){.func=read_value,.handle_count=1,
        .single={.handle=download.status_handle,.offset=0}};
    int ret=bt_gatt_read(download.conn,&download.read);
    if (ret) { release_session(ret); }
}
static void poll_handler(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&download_lock,K_FOREVER);
    if (download.conn && download.phase==DOWNLOAD_STATUS) { read_status(); }
    k_mutex_unlock(&download_lock);
}
static void timeout_handler(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&download_lock,K_FOREVER);
    if (download.conn) { complete(-ETIMEDOUT); }
    k_mutex_unlock(&download_lock);
}
static void upload_write(size_t length) {
    download.write=(struct bt_gatt_write_params){.func=written,.handle=download.command_handle,
        .data=download.command,.length=length};
    int ret=bt_gatt_write(download.conn,&download.write);
    if (ret) { release_session(ret); }
}
static void upload_next_chunk(void) {
    if (download.received==download.length) {
        download.phase=UPLOAD_SAVE;
        download.command[1]=5;
        upload_write(6);
        return;
    }
    uint16_t mtu=bt_gatt_get_mtu(download.conn);
    if (mtu<23) { release_session(-ENOTSUP); return; }
    download.chunk_length=MIN((size_t)mtu-11,sizeof(download.command)-8);
    download.chunk_length=MIN(download.chunk_length,download.length-download.received);
    download.phase=UPLOAD_CHUNK;
    download.command[1]=4;
    sys_put_le16(download.received,download.command+6);
    memcpy(download.command+8,download.buffer+download.received,download.chunk_length);
    upload_write(8+download.chunk_length);
}
static void written(struct bt_conn *conn,uint8_t error,struct bt_gatt_write_params *params) {
    ARG_UNUSED(params);
    k_mutex_lock(&download_lock,K_FOREVER);
    if (conn==download.conn) {
        if (download.phase==DOWNLOAD_RELEASE) { complete(download.result ? download.result : error ? -EIO : 0); }
        else if (download.phase==DOWNLOAD_START) {
            if (error) { complete(-EACCES); }
            else { read_status(); }
        } else if (download.phase==UPLOAD_BASE) {
            if (error) { complete(error==BT_ATT_ERR_VALUE_NOT_ALLOWED ? -ESTALE : -EACCES); }
            else {
                download.phase=UPLOAD_METADATA;
                download.command[1]=6;
                sys_put_le32(download.proposal_crc,download.command+6);
                sys_put_le16(download.length,download.command+10);
                upload_write(12);
            }
        } else if (download.phase==UPLOAD_METADATA || download.phase==UPLOAD_CHUNK) {
            if (error) { release_session(-EIO); }
            else {
                if (download.phase==UPLOAD_CHUNK) { download.received+=download.chunk_length; }
                upload_next_chunk();
            }
        } else if (download.phase==UPLOAD_SAVE) {
            if (error) { release_session(-EIO); }
            else { read_status(); }
        }
    }
    k_mutex_unlock(&download_lock);
}
static uint8_t read_value(struct bt_conn *conn,uint8_t error,struct bt_gatt_read_params *params,
                           const void *data,uint16_t length) {
    ARG_UNUSED(params);
    k_mutex_lock(&download_lock,K_FOREVER);
    uint8_t action=BT_GATT_ITER_STOP;
    if (conn!=download.conn || (download.phase!=DOWNLOAD_STATUS && download.phase!=DOWNLOAD_DATA)) {
        goto done;
    }
    if (error) { release_session(-EIO); goto done; }
    if (download.phase==DOWNLOAD_DATA) {
        if (data) {
            if (!length || length>download.length-download.received) { release_session(-EMSGSIZE); goto done; }
            memcpy(download.buffer+download.received,data,length);
            download.received+=length;
            action=BT_GATT_ITER_CONTINUE;
        } else {
            int ret=download.received==download.length &&
                keypoint_config_crc32(download.buffer,download.length)==download.revision.crc ? 0 : -EBADMSG;
            release_session(ret);
        }
        goto done;
    }
    if (data) {
        if (!length || length>sizeof(download.status)-download.status_received) { release_session(-EBADMSG); goto done; }
        memcpy(download.status+download.status_received,data,length);
        download.status_received+=length;
        action=BT_GATT_ITER_CONTINUE;
        goto done;
    }
    if (download.status_received!=sizeof(download.status) || download.status[0]!=1 ||
        sys_get_le32(download.status+2)!=sys_get_le32(download.command+2)) {
        release_session(-EBADMSG); goto done;
    }
    if (download.status[1]==(download.upload ? KEYPOINT_PROPOSAL_SAVE_QUEUED : KEYPOINT_EXPORT_QUEUED)) {
        k_work_reschedule(&download_poll,K_MSEC(100)); goto done;
    }
    if (download.status[1]!=(download.upload ? KEYPOINT_PROPOSAL_SAVED : KEYPOINT_EXPORT_AVAILABLE) ||
        sys_get_le32(download.status+6)) {
        release_session(-EIO); goto done;
    }
    size_t accepted_length=sys_get_le16(download.status+10);
    if (download.upload && accepted_length!=download.length) { release_session(-EBADMSG); goto done; }
    download.length=accepted_length;
    download.revision=(struct keypoint_config_revision){
        .generation=sys_get_le64(download.status+12),.crc=sys_get_le32(download.status+20)};
    if (!download.length || download.length>download.capacity || !download.revision.generation) {
        release_session(-EMSGSIZE); goto done;
    }
    if (download.upload) {
        int result=download.revision.generation>download.base.generation &&
            download.revision.crc==download.proposal_crc ? 0 : -EBADMSG;
        release_session(result);
        goto done;
    }
    download.phase=DOWNLOAD_DATA;
    download.read=(struct bt_gatt_read_params){.func=read_value,.handle_count=1,
        .single={.handle=download.data_handle,.offset=0}};
    int ret=bt_gatt_read(download.conn,&download.read);
    if (ret) { release_session(ret); }
done:
    k_mutex_unlock(&download_lock);
    return action;
}
static uint8_t discovered(struct bt_conn *conn,const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params) {
    ARG_UNUSED(params);
    k_mutex_lock(&download_lock,K_FOREVER);
    uint8_t action=BT_GATT_ITER_STOP;
    if (conn!=download.conn || download.phase!=DOWNLOAD_DISCOVER) { goto done; }
    if (attr) {
        const struct bt_gatt_chrc *chrc=attr->user_data;
        if (!bt_uuid_cmp(chrc->uuid,&command_uuid.uuid)) { download.command_handle=chrc->value_handle; }
        else if (!bt_uuid_cmp(chrc->uuid,&status_uuid.uuid)) { download.status_handle=chrc->value_handle; }
        else if (!bt_uuid_cmp(chrc->uuid,&data_uuid.uuid)) { download.data_handle=chrc->value_handle; }
        action=BT_GATT_ITER_CONTINUE;
    } else if (!download.command_handle || !download.status_handle || !download.data_handle) {
        complete(-ENOTSUP);
    } else {
        download.phase=download.upload ? UPLOAD_BASE : DOWNLOAD_START;
        if (download.upload) {
            download.command[1]=3;
            sys_put_le64(download.base.generation,download.command+6);
            sys_put_le32(download.base.crc,download.command+14);
        }
        download.write=(struct bt_gatt_write_params){.func=written,.handle=download.command_handle,
            .data=download.command,.length=download.upload ? 18 : 6};
        int ret=bt_gatt_write(conn,&download.write);
        if (ret) { complete(ret); }
    }
done:
    k_mutex_unlock(&download_lock);
    return action;
}
static int start_operation(struct bt_conn *left,uint32_t nonce,uint8_t *buffer,size_t capacity,
                           keypoint_config_download_callback callback,void *context,bool upload,
                           struct keypoint_config_revision base) {
    if (!left || !nonce || !buffer || !capacity || !callback) { return -EINVAL; }
    if (upload && capacity>KEYPOINT_CONFIG_MAX_SIZE) { return -EMSGSIZE; }
    if (upload && base.generation==UINT64_MAX) { return -EOVERFLOW; }
    if (!zmk_split_role_is_central() || zmk_keypoint_local_peer_kind()!=ZMK_KEYPOINT_PEER_DONGLE) { return -ENOTSUP; }
    if (bt_conn_get_security(left)<BT_SECURITY_L2) { return -EACCES; }
    struct bt_conn *expected=zmk_keypoint_input_peer_connection(1);
    bool matches=expected==left;
    if (expected) { bt_conn_unref(expected); }
    if (!matches) { return -EACCES; }
    k_mutex_lock(&download_lock,K_FOREVER);
    if (download.conn) { k_mutex_unlock(&download_lock); return -EBUSY; }
    download=(struct download_state){.phase=DOWNLOAD_DISCOVER,.conn=bt_conn_ref(left),
        .buffer=buffer,.capacity=capacity,.callback=callback,.context=context,.upload=upload,
        .base=base,.length=upload ? capacity : 0,
        .proposal_crc=upload ? keypoint_config_crc32(buffer,capacity) : 0};
    download.command[0]=1;download.command[1]=1;sys_put_le32(nonce,download.command+2);
    download.discover=(struct bt_gatt_discover_params){.func=discovered,
        .start_handle=1,.end_handle=UINT16_MAX,.type=BT_GATT_DISCOVER_CHARACTERISTIC};
    int ret=bt_gatt_discover(left,&download.discover);
    if (ret) { download.callback=NULL;complete(ret); }
    else { k_work_reschedule(&download_timeout,K_MSEC(upload ? 120000 : 30000)); }
    k_mutex_unlock(&download_lock);
    return ret;
}
int zmk_keypoint_config_download(struct bt_conn *left,uint32_t nonce,uint8_t *buffer,size_t capacity,
                                  keypoint_config_download_callback callback,void *context) {
    return start_operation(left,nonce,buffer,capacity,callback,context,false,
                            (struct keypoint_config_revision){0});
}
int zmk_keypoint_config_propose(struct bt_conn *left,uint32_t nonce,
    struct keypoint_config_revision base,const uint8_t *buffer,size_t length,
    keypoint_config_download_callback callback,void *context) {
    /* Upload never enters DOWNLOAD_DATA, and never modifies this borrowed buffer. */
    return start_operation(left,nonce,(uint8_t *)buffer,length,callback,context,true,base);
}
static void download_disconnected(struct bt_conn *conn,uint8_t reason) {
    ARG_UNUSED(reason);
    k_mutex_lock(&download_lock,K_FOREVER);
    if (conn==download.conn) { complete(-ENOTCONN); }
    k_mutex_unlock(&download_lock);
}
BT_CONN_CB_DEFINE(configuration_download_callbacks)={.disconnected=download_disconnected};
