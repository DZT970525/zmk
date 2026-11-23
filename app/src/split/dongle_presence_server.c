/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>
#include <zmk/split/dongle_presence.h>
LOG_MODULE_REGISTER(keypoint_presence_server,CONFIG_ZMK_LOG_LEVEL);
BUILD_ASSERT(CONFIG_BT_EXT_ADV_MAX_ADV_SET>=2,"Host and probe need separate advertising sets");

static struct bt_le_ext_adv *probe_adv;
static bt_addr_le_t left_address;
static bool known_left, left_connected, advertising;
K_MUTEX_DEFINE(presence_lock);

static bool address_is_left(struct bt_conn *conn) {
    return known_left && bt_addr_le_eq(bt_conn_get_dst(conn),&left_address);
}
bool zmk_keypoint_presence_is_control_connection(struct bt_conn *conn) {
    struct bt_conn_info info;
    if (!conn || bt_conn_get_info(conn,&info) || info.role!=BT_CONN_ROLE_PERIPHERAL) {
        return false;
    }
    k_mutex_lock(&presence_lock,K_FOREVER);
    bool matches=address_is_left(conn);
    k_mutex_unlock(&presence_lock);
    return matches;
}

static void update_handler(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&presence_lock,K_FOREVER);
    int ret=0;
    if (left_connected || !known_left) {
        if (probe_adv && advertising) {
            ret=bt_le_ext_adv_stop(probe_adv);
            if (!ret || ret==-EALREADY) { advertising=false; }
        }
        goto done;
    }
    if (advertising) { goto done; }
    struct bt_le_adv_param param={
        .id=BT_ID_DEFAULT,
        .options=BT_LE_ADV_OPT_CONNECTABLE|BT_LE_ADV_OPT_DIR_MODE_LOW_DUTY|
                 BT_LE_ADV_OPT_USE_IDENTITY,
        .interval_min=BT_GAP_ADV_SLOW_INT_MIN,.interval_max=BT_GAP_ADV_SLOW_INT_MAX,
        .peer=&left_address,
    };
    if (!probe_adv) { ret=bt_le_ext_adv_create(&param,NULL,&probe_adv); }
    else { ret=bt_le_ext_adv_update_param(probe_adv,&param); }
    if (!ret) { ret=bt_le_ext_adv_start(probe_adv,BT_LE_EXT_ADV_START_DEFAULT); }
    if (!ret) { advertising=true; }
done:
    k_mutex_unlock(&presence_lock);
    if (ret && ret!=-EALREADY) { LOG_WRN("Directed probe advertising deferred (%d)",ret); }
}
K_WORK_DEFINE(update_work,update_handler);

int zmk_keypoint_presence_update_left(struct bt_conn *left) {
    if (left && bt_conn_get_security(left)<BT_SECURITY_L2) { return -EACCES; }
    k_mutex_lock(&presence_lock,K_FOREVER);
    int ret=0;
    if (left) {
        const bt_addr_le_t *address=bt_conn_get_dst(left);
        if (!known_left || !bt_addr_le_eq(address,&left_address)) {
            /* Mirror calls this only after authenticated full snapshot success.
             * Persist before exposing the directed endpoint. No bond-list order
             * or advertised keyboard name determines the left identity. */
            ret=settings_save_one("keypoint_presence/left",address,sizeof(*address));
            if (!ret) { bt_addr_le_copy(&left_address,address); known_left=true; }
        }
        if (!ret) { left_connected=true; }
    } else { left_connected=false; }
    k_mutex_unlock(&presence_lock);
    if (!ret) { k_work_submit(&update_work); }
    return ret;
}

static void control_connected(struct bt_conn *conn,uint8_t error) {
    if (!error && zmk_keypoint_presence_is_control_connection(conn)) {
        k_mutex_lock(&presence_lock,K_FOREVER);
        /* The probe set stops upon connection. Keep the host set untouched. */
        advertising=false;
        k_mutex_unlock(&presence_lock);
    }
}
static void control_disconnected(struct bt_conn *conn,uint8_t reason) {
    ARG_UNUSED(reason);
    if (zmk_keypoint_presence_is_control_connection(conn)) { k_work_submit(&update_work); }
}
BT_CONN_CB_DEFINE(presence_callbacks)={
    .connected=control_connected,.disconnected=control_disconnected,
};

static int presence_settings_set(const char *name,size_t len,settings_read_cb read_cb,
                                 void *context) {
    if (strcmp(name,"left")) { return -ENOENT; }
    if (len!=sizeof(left_address)) { return -EINVAL; }
    bt_addr_le_t address;
    int ret=read_cb(context,&address,sizeof(address));
    if (ret<0) { return ret; }
    if (ret!=(int)sizeof(address)) { return -EINVAL; }
    k_mutex_lock(&presence_lock,K_FOREVER);
    left_address=address; known_left=true;
    k_mutex_unlock(&presence_lock);
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(keypoint_presence,"keypoint_presence",NULL,
                               presence_settings_set,NULL,NULL);
