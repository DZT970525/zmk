/* SPDX-License-Identifier: MIT */
#include "config_export.h"
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zmk/split/role.h>
#include <zmk/split/peer_selection.h>

#define EXPORT_UUID(n) BT_UUID_128_ENCODE(n,0xea4d,0x4fbe,0xa448,0x7d839ed0bdb0)
/* Service 407; command 408; status 409; frozen snapshot 40a.
 * Commands: [version=1, opcode, nonce LE32, payload].
 * 1 export, 2 release, 3 proposal base (generation LE64, CRC LE32),
 * 4 chunk (offset LE16, bytes), 5 save, 6 metadata (CRC LE32, length LE16).
 * Base and metadata are separate writes so even ATT MTU 23 is supported.
 * Status: version, state, nonce LE32, result LE32, length LE16,
 *         committed generation LE64, CRC32 LE32.
 * A successful start write acknowledges queueing, NOT durable/application ACK.
 * The client must wait for AVAILABLE and verify length/revision/full CRC. */
static struct bt_conn *owner;
static uint32_t owner_nonce;
K_MUTEX_DEFINE(export_owner_lock);

static bool authorized(struct bt_conn *conn) {
    return !zmk_split_role_is_central() &&
           zmk_keypoint_local_peer_kind()==ZMK_KEYPOINT_PEER_LEFT &&
           zmk_keypoint_peer_is_verified_connection(conn);
}
static void clear_owner(void) {
    if (owner) { bt_conn_unref(owner); }
    owner=NULL;
    owner_nonce=0;
}
static ssize_t export_command(struct bt_conn *conn,const struct bt_gatt_attr *attr,
                              const void *buf,uint16_t len,uint16_t offset,uint8_t flags) {
    ARG_UNUSED(attr);
    if (!authorized(conn)) { return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION); }
    if (offset) { return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET); }
    if (len<6) { return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN); }
    if (flags & (BT_GATT_WRITE_FLAG_PREPARE | BT_GATT_WRITE_FLAG_CMD)) {
        return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    }
    const uint8_t *command=buf;
    uint32_t nonce=sys_get_le32(command+2);
    if (command[0]!=1 || !nonce || command[1]<1 || command[1]>6) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    if (((command[1]==1 || command[1]==2 || command[1]==5) && len!=6) ||
        (command[1]==3 && len!=18) || (command[1]==4 && len<9) ||
        (command[1]==6 && len!=12)) { return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN); }
    k_mutex_lock(&export_owner_lock,K_FOREVER);
    struct keypoint_config_export_status status;
    int ret=zmk_keypoint_config_export_status(&status);
    if (!ret && status.state==KEYPOINT_EXPORT_IDLE) { clear_owner(); }
    if (!ret && owner && (owner!=conn || owner_nonce!=nonce)) { ret=-EACCES; }
    if (!ret && command[1]==1) {
        ret=zmk_keypoint_config_export_start(nonce);
        if (!ret && !owner) { owner=bt_conn_ref(conn); owner_nonce=nonce; }
    } else if (!ret && command[1]==3) {
        struct keypoint_config_revision base={sys_get_le64(command+6),sys_get_le32(command+14)};
        ret=zmk_keypoint_config_proposal_start(nonce,base);
        if (!ret && !owner) { owner=bt_conn_ref(conn); owner_nonce=nonce; }
    } else if (!ret) {
        if (owner!=conn) { ret=-EACCES; }
        else if (command[1]==2) {
            ret=zmk_keypoint_config_export_release(nonce);
            if (!ret) { clear_owner(); }
        } else if (command[1]==4) {
            ret=zmk_keypoint_config_proposal_chunk(nonce,sys_get_le16(command+6),command+8,len-8);
        } else if (command[1]==5) {
            ret=zmk_keypoint_config_proposal_save(nonce);
        } else {
            ret=zmk_keypoint_config_proposal_metadata(nonce,sys_get_le32(command+6),sys_get_le16(command+10));
        }
    }
    k_mutex_unlock(&export_owner_lock);
    if (ret) {
        return BT_GATT_ERR(ret==-EACCES ? BT_ATT_ERR_AUTHORIZATION :
                          ret==-ESTALE || ret==-EBADMSG || ret==-EMSGSIZE || ret==-EINVAL ?
                          BT_ATT_ERR_VALUE_NOT_ALLOWED :
                                         BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    }
    return len;
}
static ssize_t export_status_read(struct bt_conn *conn,const struct bt_gatt_attr *attr,
                                  void *buf,uint16_t len,uint16_t offset) {
    if (!authorized(conn)) { return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION); }
    k_mutex_lock(&export_owner_lock,K_FOREVER);
    struct keypoint_config_export_status status;
    int ret=zmk_keypoint_config_export_status(&status);
    if (!ret && owner && owner!=conn) { ret=-EACCES; }
    k_mutex_unlock(&export_owner_lock);
    if (ret) { return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES); }
    uint8_t value[24]={1,status.state};
    sys_put_le32(status.nonce,value+2);
    sys_put_le32((uint32_t)status.result,value+6);
    sys_put_le16(status.length,value+10);
    sys_put_le64(status.revision.generation,value+12);
    sys_put_le32(status.revision.crc,value+20);
    return bt_gatt_attr_read(conn,attr,buf,len,offset,value,sizeof(value));
}
static ssize_t export_data_read(struct bt_conn *conn,const struct bt_gatt_attr *attr,
                                void *buf,uint16_t len,uint16_t offset) {
    ARG_UNUSED(attr);
    if (!authorized(conn)) { return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION); }
    k_mutex_lock(&export_owner_lock,K_FOREVER);
    struct keypoint_config_export_status status;
    int ret=zmk_keypoint_config_export_status(&status);
    if (!ret && (owner!=conn || status.nonce!=owner_nonce ||
                 status.state!=KEYPOINT_EXPORT_AVAILABLE)) { ret=-EACCES; }
    if (!ret && offset>status.length) { ret=-ERANGE; }
    size_t count=ret ? 0 : MIN((size_t)len,status.length-offset);
    if (!ret && count) {
        ret=zmk_keypoint_config_export_read(owner_nonce,offset,buf,count);
    }
    k_mutex_unlock(&export_owner_lock);
    if (ret) {
        return BT_GATT_ERR(ret==-ERANGE ? BT_ATT_ERR_INVALID_OFFSET :
                          ret==-EACCES ? BT_ATT_ERR_AUTHORIZATION :
                                        BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    }
    return count;
}
static void export_disconnected(struct bt_conn *conn,uint8_t reason) {
    ARG_UNUSED(reason);
    k_mutex_lock(&export_owner_lock,K_FOREVER);
    if (conn==owner) {
        /* If the worker is currently writing, expiry releases the staging
         * buffer later. Disconnect never frees it while a worker uses it. */
        zmk_keypoint_config_export_release(owner_nonce);
        clear_owner();
    }
    k_mutex_unlock(&export_owner_lock);
}
BT_CONN_CB_DEFINE(export_connection_callbacks)={.disconnected=export_disconnected};
BT_GATT_SERVICE_DEFINE(keypoint_configuration_export,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(EXPORT_UUID(0xcf03e407))),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(EXPORT_UUID(0xcf03e408)),BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE_ENCRYPT,NULL,export_command,NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(EXPORT_UUID(0xcf03e409)),BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ_ENCRYPT,export_status_read,NULL,NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(EXPORT_UUID(0xcf03e40a)),BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ_ENCRYPT,export_data_read,NULL,NULL));
