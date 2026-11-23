/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keypoint_relay.h>
#include <zmk/workqueue.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>

struct telemetry_slot { uint8_t bytes[12];int64_t received;uint32_t sequence;bool dirty; };
static struct telemetry_slot slots[2];
static bool clear_pending;
static uint32_t host_generation;
K_MUTEX_DEFINE(telemetry_lock);
static void forward_work_cb(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(forward_work,forward_work_cb);
static void schedule(int delay) {
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(),&forward_work,K_MSEC(delay));
}
static bool valid(const uint8_t *b,size_t len) {
    return len==12 && (b[0]==1 || b[0]==2) && !b[3] &&
        (b[1]<=100 || b[1]==255) && (b[2]<=100 || b[2]==255) &&
        sys_get_le32(b+4)<=604800 && sys_get_le32(b+8)<=604800 &&
        (b[0]!=2 || (!sys_get_le32(b+4) && !sys_get_le32(b+8)));
}
static int accept(const uint8_t *b,size_t len) {
    if(!valid(b,len))return -EINVAL;
    if(!zmk_keypoint_relay_connected())return -ENOTCONN;
    k_mutex_lock(&telemetry_lock,K_FOREVER);
    struct telemetry_slot *s=&slots[b[0]-1];
    memcpy(s->bytes,b,12);s->received=k_uptime_get();s->sequence++;s->dirty=true;
    k_mutex_unlock(&telemetry_lock);schedule(0);return 0;
}
static void forward_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&telemetry_lock,K_FOREVER);bool clear=clear_pending;uint32_t generation=host_generation;k_mutex_unlock(&telemetry_lock);
    if(clear) {
        if(zmk_keypoint_relay_send_telemetry(NULL,0)) { schedule(1000);return; }
        k_mutex_lock(&telemetry_lock,K_FOREVER);if(generation==host_generation)clear_pending=false;k_mutex_unlock(&telemetry_lock);
    }
    for(int i=0;i<2;i++) {
        k_mutex_lock(&telemetry_lock,K_FOREVER);struct telemetry_slot s=slots[i];bool changed=generation!=host_generation;k_mutex_unlock(&telemetry_lock);
        if(changed) { schedule(0);return; }
        if(!s.dirty)continue;
        uint32_t age=(k_uptime_get()-s.received)/1000;
        if(age >= (i?15:360)) {
            k_mutex_lock(&telemetry_lock,K_FOREVER);if(slots[i].sequence==s.sequence)slots[i].dirty=false;k_mutex_unlock(&telemetry_lock);continue;
        }
        if(!i)for(int j=4;j<=8;j+=4) { uint32_t sec=sys_get_le32(s.bytes+j);sys_put_le32(sec>age?sec-age:0,s.bytes+j); }
        int ret=zmk_keypoint_relay_send_telemetry(s.bytes,12);
        k_mutex_lock(&telemetry_lock,K_FOREVER);
        if(!ret && slots[i].sequence==s.sequence)slots[i].dirty=false;
        k_mutex_unlock(&telemetry_lock);
        if(ret) { schedule(1000);return; }
    }
}
static int host_changed(const zmk_event_t *event) {
    ARG_UNUSED(event);
    k_mutex_lock(&telemetry_lock,K_FOREVER);
    for(int i=0;i<2;i++) { slots[i].dirty=false;slots[i].sequence++; }
    host_generation++;clear_pending=true;k_mutex_unlock(&telemetry_lock);schedule(0);
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(dongle_telemetry_host,host_changed);
ZMK_SUBSCRIPTION(dongle_telemetry_host,zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(dongle_telemetry_host,zmk_endpoint_changed);
static ssize_t telemetry_write(struct bt_conn *conn,const struct bt_gatt_attr *attr,
                               const void *buf,uint16_t len,uint16_t offset,uint8_t flags) {
    ARG_UNUSED(attr);
    struct bt_conn *active=zmk_ble_active_profile_conn();
    bool allowed=active && active==conn && zmk_endpoint_get_selected().transport==ZMK_TRANSPORT_BLE;
    if(active)bt_conn_unref(active);
    if(!allowed)return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    if(offset || (flags & BT_GATT_WRITE_FLAG_PREPARE))return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    return accept(buf,len)?BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED):len;
}
BT_GATT_SERVICE_DEFINE(dongle_telemetry_service,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0xf5d41000,0x6d2f,0x4f4b,0x9b2a,0x2f4a8e8c0001))),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0xf5d41001,0x6d2f,0x4f4b,0x9b2a,0x2f4a8e8c0001)),
        BT_GATT_CHRC_WRITE|BT_GATT_CHRC_WRITE_WITHOUT_RESP,BT_GATT_PERM_WRITE_ENCRYPT,NULL,telemetry_write,NULL));
#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
#include <zmk/studio/custom.h>
#include <pb_encode.h>
static uint8_t reply[8];
static bool encode_reply(pb_ostream_t *stream,const pb_field_t *field,void *const *arg) {
    ARG_UNUSED(arg);return pb_encode_tag_for_field(stream,field)&&pb_encode_string(stream,reply,sizeof(reply));
}
static bool telemetry_rpc(const zmk_custom_CallRequest *request,pb_callback_t *output) {
    int ret=zmk_endpoint_get_selected().transport==ZMK_TRANSPORT_USB ?
        accept(request->payload.bytes,request->payload.size):-EACCES;
    memset(reply,0,sizeof(reply));reply[0]=1;reply[7]=1;sys_put_le32(ret,reply+1);
    output->funcs.encode=encode_reply;output->arg=NULL;return true;
}
static struct zmk_rpc_custom_subsystem_meta meta={.security=ZMK_STUDIO_RPC_HANDLER_UNSECURED};
ZMK_RPC_CUSTOM_SUBSYSTEM(zitaotech_keypoint_telemetry,&meta,telemetry_rpc);
#endif
