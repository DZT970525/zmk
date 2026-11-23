/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zmk/split/role.h>
#include <zmk/split/peer_selection.h>

/* An authenticated dongle can read/write the LEFT's authoritative permission.
 * Reads: version, enabled, boot role, prepared, target (0xff if absent), manual.
 * Writes: [1,1,enabled] persist permission; [1,2] prepare manual toggle;
 * [1,3,target] confirm after right handoff ACK; [1,4] cancel before handoff.
 * Toggle ACK means preparation accepted, never that the topology changed.
 * The dongle must observe prepared/manual state and switch the right before
 * sending confirm. Persist/prepare failure must not be displayed as success.
 */
static bool authorized(struct bt_conn *conn) {
    return !zmk_split_role_is_central() && zmk_keypoint_peer_is_verified_connection(conn);
}
static ssize_t topology_read(struct bt_conn *conn,const struct bt_gatt_attr *attr,
                             void *buffer,uint16_t length,uint16_t offset) {
    if (!authorized(conn)) { return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION); }
    bool target=false;
    bool prepared=!zmk_split_role_transition_target(&target);
    const uint8_t status[]={1,zmk_split_role_manual_switch_enabled(),
        zmk_split_role_is_central(),prepared,prepared ? target : UINT8_MAX,
        zmk_split_role_transition_is_manual()};
    return bt_gatt_attr_read(conn,attr,buffer,length,offset,status,sizeof(status));
}
static ssize_t topology_write(struct bt_conn *conn,const struct bt_gatt_attr *attr,
                              const void *buffer,uint16_t length,uint16_t offset,uint8_t flags) {
    ARG_UNUSED(attr);
    if (!authorized(conn)) { return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION); }
    if (offset) { return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET); }
    if (flags & (BT_GATT_WRITE_FLAG_PREPARE|BT_GATT_WRITE_FLAG_CMD)) {
        return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    }
    if (length<2) { return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN); }
    const uint8_t *command=buffer;
    if (command[0]!=1 || command[1]<1 || command[1]>5) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    unsigned int expected=(command[1]==1 || command[1]==3) ? 3 : 2;
    if (length!=expected) { return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN); }
    if (expected==3 && command[2]>1) { return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED); }
    int ret;
    if (command[1]==1) {
        ret=zmk_split_role_set_manual_switch_enabled(command[2]!=0);
    } else if (command[1]==2) {
        ret=zmk_split_role_toggle();
    } else if (!zmk_split_role_transition_is_manual()) {
        ret=-EPERM;
    } else if (command[1]==5) {
        ret=zmk_split_role_begin_handoff();
    } else if (command[1]==4) {
        ret=zmk_split_role_cancel_transition();
    } else {
        bool target=false;
        ret=zmk_split_role_transition_target(&target);
        if (!ret && target!=(command[2]!=0)) { ret=-EINVAL; }
        if (!ret) { ret=zmk_split_role_confirm_transition(); }
    }
    if (ret) {
        return BT_GATT_ERR(ret==-EPERM ? BT_ATT_ERR_AUTHORIZATION :
            ret==-EINVAL ? BT_ATT_ERR_VALUE_NOT_ALLOWED : BT_ATT_ERR_UNLIKELY);
    }
    return length;
}
BT_GATT_SERVICE_DEFINE(keypoint_topology_control,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(
        BT_UUID_128_ENCODE(0xcf03e40b,0xea4d,0x4fbe,0xa448,0x7d839ed0bdb0))),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(
        BT_UUID_128_ENCODE(0xcf03e40c,0xea4d,0x4fbe,0xa448,0x7d839ed0bdb0)),
        BT_GATT_CHRC_READ|BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_READ_ENCRYPT|BT_GATT_PERM_WRITE_ENCRYPT,topology_read,topology_write,NULL));
