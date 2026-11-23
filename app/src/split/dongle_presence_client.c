/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zmk/split/role.h>
#include <zmk/split/peer_selection.h>
#include <zmk/split/dongle_presence.h>

static struct bt_uuid_128 identity_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0xcf03e405,0xea4d,0x4fbe,0xa448,0x7d839ed0bdb0));
static struct bt_conn *probe;
static struct bt_gatt_read_params read_params;
static bt_addr_le_t candidate;
static bool pending, connected, reading, disconnecting;
static int64_t deadline, retry_after, verified_until;
K_MUTEX_DEFINE(probe_lock);
static void probe_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(probe_work,probe_handler);

static void bonded(const struct bt_bond_info *info,void *context) {
    bool *found=context;
    if (bt_addr_le_eq(&info->addr,&candidate)) { *found=true; }
}

bool zmk_keypoint_presence_probe_connection(struct bt_conn *conn) {
    k_mutex_lock(&probe_lock,K_FOREVER);
    bool matches=conn && conn==probe;
    k_mutex_unlock(&probe_lock);
    return matches;
}
bool zmk_keypoint_presence_dongle_available(void) {
    k_mutex_lock(&probe_lock,K_FOREVER);
    bool available=verified_until>k_uptime_get();
    k_mutex_unlock(&probe_lock);
    return available;
}
bool zmk_keypoint_presence_probe_advertisement(const bt_addr_le_t *address) {
    bt_addr_le_t saved;
    if (!zmk_split_role_is_central() || zmk_split_role_standalone_selected() ||
        zmk_keypoint_peer_target_address(&saved) || !bt_addr_le_eq(address,&saved)) {
        return false;
    }
    k_mutex_lock(&probe_lock,K_FOREVER);
    if (!probe && !pending && k_uptime_get()>=retry_after) {
        candidate=*address;
        bool found=false;
        bt_foreach_bond(BT_ID_DEFAULT,bonded,&found);
        if (found) { pending=true; k_work_reschedule(&probe_work,K_NO_WAIT); }
    }
    k_mutex_unlock(&probe_lock);
    /* Never let a dongle advertisement consume the right peripheral slot. */
    return true;
}

static uint8_t identity_received(struct bt_conn *conn,uint8_t error,
                                struct bt_gatt_read_params *params,
                                const void *data,uint16_t length) {
    ARG_UNUSED(params);
    k_mutex_lock(&probe_lock,K_FOREVER);
    if (conn==probe) {
        const uint8_t *identity=data;
        if (!error && data && length==3 && identity[0]==1 &&
            identity[1]==ZMK_KEYPOINT_PEER_DONGLE && identity[2]==1 &&
            bt_conn_get_security(conn)>=BT_SECURITY_L2) {
            verified_until=k_uptime_get()+5000;
        }
        deadline=0;
        k_work_reschedule(&probe_work,K_NO_WAIT);
    }
    k_mutex_unlock(&probe_lock);
    return BT_GATT_ITER_STOP;
}

static void probe_handler(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&probe_lock,K_FOREVER);
    int64_t now=k_uptime_get();
    if (pending && !probe) {
        pending=false;
        retry_after=now+3000;
        if (!zmk_split_role_is_central() || zmk_split_role_standalone_selected()) { goto done; }
        int ret=zmk_keypoint_presence_scan_pause();
        if (ret && ret!=-EALREADY) { goto resume; }
        deadline=now+8000;
        ret=bt_conn_le_create(&candidate,BT_CONN_LE_CREATE_CONN,
                              BT_LE_CONN_PARAM_DEFAULT,&probe);
        if (ret) { probe=NULL; goto resume; }
        goto poll;
    }
    if (!probe) { goto done; }
    if (now>=deadline || !zmk_split_role_is_central()) {
        if (!disconnecting) {
            int ret=bt_conn_disconnect(probe,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            if (!ret) { disconnecting=true; }
        }
        goto poll;
    }
    if (connected && !reading) {
        if (bt_conn_get_security(probe)<BT_SECURITY_L2) {
            if (bt_conn_set_security(probe,BT_SECURITY_L2)) { deadline=0; }
        } else {
            read_params=(struct bt_gatt_read_params){
                .func=identity_received,.handle_count=0,
                .by_uuid={.start_handle=1,.end_handle=UINT16_MAX,
                          .uuid=&identity_uuid.uuid}};
            reading=true;
            if (bt_gatt_read(probe,&read_params)) { deadline=0; }
        }
    }
poll:
    k_work_reschedule(&probe_work,K_MSEC(250));
    goto done;
resume:
    zmk_keypoint_presence_scan_resume();
done:
    k_mutex_unlock(&probe_lock);
}

static void probe_connected(struct bt_conn *conn,uint8_t error) {
    k_mutex_lock(&probe_lock,K_FOREVER);
    if (conn==probe) {
        if (error) {
            bt_conn_unref(probe); probe=NULL;
            connected=reading=disconnecting=false;
            zmk_keypoint_presence_scan_resume();
        } else { connected=true; }
        k_work_reschedule(&probe_work,K_NO_WAIT);
    }
    k_mutex_unlock(&probe_lock);
}
static void probe_disconnected(struct bt_conn *conn,uint8_t reason) {
    ARG_UNUSED(reason);
    k_mutex_lock(&probe_lock,K_FOREVER);
    if (conn==probe) {
        bt_conn_unref(probe); probe=NULL;
        connected=reading=disconnecting=false;
        retry_after=k_uptime_get()+3000;
        zmk_keypoint_presence_scan_resume();
    }
    k_mutex_unlock(&probe_lock);
}
BT_CONN_CB_DEFINE(keypoint_probe_callbacks)={
    .connected=probe_connected,.disconnected=probe_disconnected,
};
