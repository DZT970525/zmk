/* SPDX-License-Identifier: MIT */
#include "topology_control_client.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zmk/split/peer_selection.h>
static const struct bt_uuid_128 control_uuid=BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0xcf03e40c,0xea4d,0x4fbe,0xa448,0x7d839ed0bdb0));
static struct {
    struct bt_conn *conn;
    struct bt_gatt_discover_params discover;
    struct bt_gatt_read_params read;
    struct bt_gatt_write_params write;
    uint8_t command[3],bytes[6],opcode;
    size_t received;
    int64_t deadline;
    uint16_t handle;
    keypoint_topology_callback callback;
    void *context;
} control;
K_MUTEX_DEFINE(control_lock);
static void timeout_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(control_timeout,timeout_handler);
/* All callers hold the recursive mutex; callbacks run outside it. */
static void complete(int result) {
    if (!control.conn) return;
    struct bt_conn *conn=control.conn;
    keypoint_topology_callback callback=control.callback;
    void *context=control.context;
    struct keypoint_topology_status status={0};
    if (!result) status=(struct keypoint_topology_status){
        .enabled=control.bytes[1],.central=control.bytes[2],
        .prepared=control.bytes[3],.target=control.bytes[4],.manual=control.bytes[5]};
    k_work_cancel_delayable(&control_timeout);
    bt_gatt_cancel(conn,&control.discover);
    bt_gatt_cancel(conn,&control.read);
    bt_gatt_cancel(conn,&control.write);
    control.conn=NULL;
    bt_conn_unref(conn);
    k_mutex_unlock(&control_lock);
    if (callback) callback(result,status,context);
    k_mutex_lock(&control_lock,K_FOREVER);
}
static void timeout_handler(struct k_work *work) {
    ARG_UNUSED(work);k_mutex_lock(&control_lock,K_FOREVER);
    if (control.conn) {
        int64_t remaining=control.deadline-k_uptime_get();
        if (remaining>0) k_work_reschedule(&control_timeout,K_MSEC(remaining));
        else complete(-ETIMEDOUT);
    }
    k_mutex_unlock(&control_lock);
}
static uint8_t received(struct bt_conn *conn,uint8_t error,
    struct bt_gatt_read_params *params,const void *data,uint16_t length) {
    ARG_UNUSED(params);k_mutex_lock(&control_lock,K_FOREVER);
    uint8_t next=BT_GATT_ITER_STOP;
    if (conn!=control.conn) goto done;
    if (error) {complete(-EIO);goto done;}
    if (data) {
        if (length>sizeof(control.bytes)-control.received) {complete(-EBADMSG);goto done;}
        memcpy(control.bytes+control.received,data,length);control.received+=length;
        next=BT_GATT_ITER_CONTINUE;
    } else {
        bool valid=control.received==6 && control.bytes[0]==1;
        for (int i=1;i<6;i++) if (i!=4 && control.bytes[i]>1) valid=false;
        if (control.bytes[4]!=255 && control.bytes[4]>1) valid=false;
        if (control.bytes[3] && control.bytes[4]==255) valid=false;
        complete(valid ? 0 : -EBADMSG);
    }
done:k_mutex_unlock(&control_lock);return next;
}
static void read_status(void) {
    control.received=0;
    control.read=(struct bt_gatt_read_params){.func=received,.handle_count=1,
        .single={.handle=control.handle,.offset=0}};
    int ret=bt_gatt_read(control.conn,&control.read);if (ret) complete(ret);
}
static void written(struct bt_conn *conn,uint8_t error,struct bt_gatt_write_params *params) {
    ARG_UNUSED(params);k_mutex_lock(&control_lock,K_FOREVER);
    if (conn==control.conn) {
        if (error) complete(-EIO);
        else if (control.opcode==3) {memset(control.bytes,0,sizeof(control.bytes));complete(0);}
        else read_status();
    }
    k_mutex_unlock(&control_lock);
}
static uint8_t found(struct bt_conn *conn,const struct bt_gatt_attr *attr,
    struct bt_gatt_discover_params *params) {
    ARG_UNUSED(params);k_mutex_lock(&control_lock,K_FOREVER);
    if (conn!=control.conn) goto done;
    if (!attr) {complete(-ENOTSUP);goto done;}
    control.handle=((const struct bt_gatt_chrc *)attr->user_data)->value_handle;
    if (!control.opcode) read_status();
    else {
        control.write=(struct bt_gatt_write_params){.func=written,.handle=control.handle,
            .data=control.command,.length=(control.opcode==1 || control.opcode==3) ? 3 : 2};
        int ret=bt_gatt_write(conn,&control.write);if (ret) complete(ret);
    }
done:k_mutex_unlock(&control_lock);return BT_GATT_ITER_STOP;
}
int zmk_keypoint_topology_remote(struct bt_conn *left,uint8_t opcode,bool value,
    keypoint_topology_callback callback,void *context) {
    if (!left || opcode>5 || !callback) return -EINVAL;
    if (bt_conn_get_security(left)<BT_SECURITY_L2) return -EACCES;
    struct bt_conn *verified=zmk_keypoint_input_peer_connection(1);
    bool trusted=verified==left;if (verified) bt_conn_unref(verified);
    if (!trusted) return -EACCES;
    k_mutex_lock(&control_lock,K_FOREVER);
    if (control.conn) {k_mutex_unlock(&control_lock);return -EBUSY;}
    control.conn=bt_conn_ref(left);control.opcode=opcode;
    control.callback=callback;control.context=context;
    control.command[0]=1;control.command[1]=opcode;control.command[2]=value;
    control.discover=(struct bt_gatt_discover_params){.uuid=&control_uuid.uuid,.func=found,
        .start_handle=1,.end_handle=UINT16_MAX,.type=BT_GATT_DISCOVER_CHARACTERISTIC};
    control.deadline=k_uptime_get()+8000;
    k_work_reschedule(&control_timeout,K_MSEC(8000));
    int ret=bt_gatt_discover(left,&control.discover);
    if (ret) {control.callback=NULL;complete(ret);}
    k_mutex_unlock(&control_lock);return ret;
}
