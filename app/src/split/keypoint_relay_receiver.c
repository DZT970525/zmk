/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zmk/keypoint_relay.h>
#include <zmk/hid.h>
#include <zmk/ble.h>
#include <dt-bindings/zmk/bt.h>
#include <dt-bindings/zmk/outputs.h>
#include <zmk/endpoints.h>
#include <zmk/activity.h>
#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/dongle_battery_state_changed.h>

ZMK_EVENT_IMPL(zmk_keypoint_relay_changed);
static const struct bt_uuid_128 service_uuid=BT_UUID_INIT_128(KEYPOINT_RELAY_SERVICE);
static const struct bt_uuid_128 report_uuid=BT_UUID_INIT_128(KEYPOINT_RELAY_REPORT);
static const struct bt_uuid_128 control_uuid=BT_UUID_INIT_128(KEYPOINT_RELAY_CONTROL);
static struct bt_conn *source;
static struct k_spinlock source_lock;
static bt_addr_le_t remembered;
static bool has_peer, scanning, discovering;
static atomic_t link_ready, clear_pending;
static struct bt_gatt_discover_params discovery;
static struct bt_gatt_subscribe_params subscription;
static struct bt_gatt_discover_params ccc_discovery;
static struct bt_gatt_write_params hello_write;
static uint16_t report_handle,control_handle;
static const uint8_t hello[]={0x80,1,sizeof(struct zmk_hid_keyboard_report_body),
    sizeof(struct zmk_hid_consumer_report_body),sizeof(struct zmk_hid_mouse_report_body)};
static struct keypoint_relay_status status;
static struct k_spinlock status_lock;
K_MSGQ_DEFINE(packets,sizeof(struct keypoint_relay_packet),64,4);
static void connect_work_handler(struct k_work *work);
static void retry_work_handler(struct k_work *work);
static void receive_work_handler(struct k_work *work);
static void discover_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(retry_work,retry_work_handler);
K_WORK_DELAYABLE_DEFINE(discover_work,discover_work_handler);
K_WORK_DEFINE(connect_work,connect_work_handler);
K_WORK_DEFINE(receive_work,receive_work_handler);
static bt_addr_le_t found_address;
static atomic_t connecting;
bool zmk_keypoint_relay_connected(void) { return atomic_get(&link_ready); }
bool zmk_keypoint_relay_is_connection(struct bt_conn *conn) { return conn && conn==source; }
int zmk_keypoint_relay_send_telemetry(const uint8_t *bytes,size_t length) {
    if(length!=0 && length!=12)return -EINVAL;
    uint8_t packet[13];packet[0]=length?0x82:0x83;
    if(length)memcpy(packet+1,bytes,length);
    k_spinlock_key_t key=k_spin_lock(&source_lock);
    struct bt_conn *conn=source && atomic_get(&link_ready) ? bt_conn_ref(source) : NULL;
    uint16_t handle=control_handle;
    k_spin_unlock(&source_lock,key);
    if(!conn)return -ENOTCONN;
    int ret=bt_gatt_write_without_response(conn,handle,packet,length+1,false);
    bt_conn_unref(conn);return ret;
}
struct keypoint_relay_status zmk_keypoint_relay_status(void) {
    k_spinlock_key_t key=k_spin_lock(&status_lock);
    struct keypoint_relay_status value=status;
    k_spin_unlock(&status_lock,key);
    return value;
}
static int settings_set(const char *name,size_t length,settings_read_cb read,void *arg) {
    if (strcmp(name,"peer") || length!=sizeof(remembered)) return -ENOENT;
    int ret=read(arg,&remembered,sizeof(remembered));
    if (ret==sizeof(remembered)) has_peer=true;
    return ret<0 ? ret : 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(keypoint_relay,"keypoint_relay",NULL,settings_set,NULL,NULL);
static void receive_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (atomic_set(&clear_pending,0)) {
        zmk_endpoint_clear_reports();
        zmk_endpoint_send_mouse_report();
        k_spinlock_key_t key=k_spin_lock(&status_lock);
        memset(&status,0,sizeof(status));
        k_spin_unlock(&status_lock,key);
        raise_zmk_dongle_battery_state_changed((struct zmk_dongle_battery_state_changed){.side=0,.level=0});
        raise_zmk_dongle_battery_state_changed((struct zmk_dongle_battery_state_changed){.side=1,.level=0});
        raise_zmk_keypoint_relay_changed((struct zmk_keypoint_relay_changed){.connected=false});
    }
    struct keypoint_relay_packet packet;
    while (!k_msgq_get(&packets,&packet,K_NO_WAIT)) {
        if (!atomic_get(&link_ready)) continue;
        switch (packet.data[0]) {
        case 1:
            if (packet.length!=sizeof(struct zmk_hid_keyboard_report_body)+1) break;
            zmk_activity_note();
            memcpy(&zmk_hid_get_keyboard_report()->body,packet.data+1,packet.length-1);
            {
                k_spinlock_key_t key = k_spin_lock(&status_lock);
                uint8_t mods = zmk_hid_get_keyboard_report()->body.modifiers;
                bool changed = status.mods != mods;
                status.mods = mods;
                k_spin_unlock(&status_lock, key);
                if (changed) raise_zmk_keypoint_relay_changed((struct zmk_keypoint_relay_changed){.connected=true});
            }
            zmk_endpoint_send_report(0x07);
            break;
        case 2:
            if (packet.length!=sizeof(struct zmk_hid_consumer_report_body)+1) break;
            zmk_activity_note();
            memcpy(&zmk_hid_get_consumer_report()->body,packet.data+1,packet.length-1);
            zmk_endpoint_send_report(0x0c);
            break;
        case 3:
            if (packet.length!=sizeof(struct zmk_hid_mouse_report_body)+1) break;
            zmk_activity_note();
            memcpy(&zmk_hid_get_mouse_report()->body,packet.data+1,packet.length-1);
            zmk_endpoint_send_mouse_report();
            /* Relative deltas must not be replayed by an endpoint switch. */
            zmk_hid_mouse_movement_set(0,0); zmk_hid_mouse_scroll_set(0,0);
            break;
        case 6:
            if (packet.length != 2) break;
            zmk_activity_note();
            switch (packet.data[1]) {
            case OUT_TOG: zmk_endpoint_toggle_preferred_transport(); break;
            case OUT_USB: zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_USB); break;
            case OUT_BLE: zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_BLE); break;
            case OUT_NONE: zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_NONE); break;
            default: break;
            }
            break;
        case 5:
            if (packet.length != 3) break;
            zmk_activity_note();
            switch (packet.data[1]) {
            case BT_NXT_CMD: zmk_ble_prof_next(); break;
            case BT_PRV_CMD: zmk_ble_prof_prev(); break;
            case BT_SEL_CMD:
                if (packet.data[2] < ZMK_BLE_PROFILE_COUNT)
                    zmk_ble_prof_select(packet.data[2]);
                break;
            default: break;
            }
            break;
        case 4:
            if (packet.length!=20) break;
            k_spinlock_key_t key=k_spin_lock(&status_lock);
            struct keypoint_relay_status previous = status;
            status.left=packet.data[1]; status.right=packet.data[2];
            status.wpm=packet.data[3]; status.layer=packet.data[4]; status.mods=packet.data[5];
            memcpy(status.label,packet.data+6,14); status.label[14]='\0';
            k_spin_unlock(&status_lock,key);
            if (previous.left != packet.data[1]) raise_zmk_dongle_battery_state_changed((struct zmk_dongle_battery_state_changed){.side=0,.level=packet.data[1]});
            if (previous.right != packet.data[2]) raise_zmk_dongle_battery_state_changed((struct zmk_dongle_battery_state_changed){.side=1,.level=packet.data[2]});
            if (previous.wpm != packet.data[3] || previous.layer != packet.data[4] ||
                previous.mods != packet.data[5] || memcmp(previous.label, packet.data+6, 14))
                raise_zmk_keypoint_relay_changed((struct zmk_keypoint_relay_changed){.connected=true});
            break;
        }
    }
}
static uint8_t notification(struct bt_conn *conn,struct bt_gatt_subscribe_params *params,
                            const void *data,uint16_t length) {
    ARG_UNUSED(params);
    if (!data) {
        if (conn == source) {
            atomic_clear(&link_ready);
            atomic_set(&clear_pending, 1);
            k_work_submit(&receive_work);
            bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
        return BT_GATT_ITER_STOP;
    }
    if (conn!=source || length<2 || length>20) return BT_GATT_ITER_CONTINUE;
    struct keypoint_relay_packet packet={.length=length};
    memcpy(packet.data,data,length);
    if (k_msgq_put(&packets,&packet,K_NO_WAIT)) {
        /* Loss of an ordered keyboard stream must never leave a held key. */
        atomic_clear(&link_ready); atomic_set(&clear_pending,1);
        bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
    k_work_submit(&receive_work);
    return BT_GATT_ITER_CONTINUE;
}
static void subscribed(struct bt_conn *conn,uint8_t error,struct bt_gatt_subscribe_params *params) {
    ARG_UNUSED(params);
    if (conn!=source) return;
    if (error) { bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN); return; }
    atomic_set(&link_ready,1);
    /* A newly connected keyboard should leave the idle splash immediately.
     * Periodic status packets deliberately do not count as user activity. */
    zmk_activity_note();
    bt_addr_le_copy(&remembered,bt_conn_get_dst(conn)); has_peer=true;
    /* Persist on the system worker, never erase flash in the Bluetooth callback. */
    k_work_reschedule(&retry_work,K_NO_WAIT);
}
static void hello_written(struct bt_conn *conn,uint8_t error,struct bt_gatt_write_params *params) {
    ARG_UNUSED(params);
    if (conn!=source) return;
    if (error) { bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN); return; }
    subscription=(struct bt_gatt_subscribe_params){.notify=notification,.subscribe=subscribed,
        .value=BT_GATT_CCC_NOTIFY,.value_handle=report_handle,.ccc_handle=BT_GATT_AUTO_DISCOVER_CCC_HANDLE,
        .end_handle=UINT16_MAX,.disc_params=&ccc_discovery};
    int ret=bt_gatt_subscribe(conn,&subscription);
    if (ret && ret!=-EALREADY) bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}
static uint8_t discovered(struct bt_conn *conn,const struct bt_gatt_attr *attr,
                          struct bt_gatt_discover_params *params) {
    ARG_UNUSED(params);
    if (conn!=source) return BT_GATT_ITER_STOP;
    if (attr) {
        const struct bt_gatt_chrc *chrc=attr->user_data;
        if (!bt_uuid_cmp(chrc->uuid,&report_uuid.uuid)) report_handle=chrc->value_handle;
        if (!bt_uuid_cmp(chrc->uuid,&control_uuid.uuid)) control_handle=chrc->value_handle;
        return BT_GATT_ITER_CONTINUE;
    }
    discovering=false;
    if (!report_handle || !control_handle) { bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN); return BT_GATT_ITER_STOP; }
    hello_write=(struct bt_gatt_write_params){.func=hello_written,.handle=control_handle,.data=hello,.length=sizeof(hello)};
    if (bt_gatt_write(conn,&hello_write)) bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    return BT_GATT_ITER_STOP;
}
static void discover_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!source || discovering || atomic_get(&link_ready) || bt_conn_get_security(source)<BT_SECURITY_L2) return;
    discovering=true; report_handle=control_handle=0;
    discovery=(struct bt_gatt_discover_params){.func=discovered,.type=BT_GATT_DISCOVER_CHARACTERISTIC,
        .start_handle=1,.end_handle=UINT16_MAX};
    if (bt_gatt_discover(source,&discovery)) {
        discovering=false; k_work_reschedule(&discover_work,K_MSEC(100));
    }
}
static bool advertisement(struct bt_data *data,void *context) {
    bool *match=context;
    if (data->type==BT_DATA_UUID128_ALL || data->type==BT_DATA_UUID128_SOME) {
        for (size_t offset=0;offset+16<=data->data_len;offset+=16)
            if (!memcmp(data->data+offset,service_uuid.val,16)) *match=true;
    }
    return true;
}
static void scan_received(const struct bt_le_scan_recv_info *info,struct net_buf_simple *buffer) {
    if (source || !(info->adv_props & BT_GAP_ADV_PROP_CONNECTABLE)) return;
    if (has_peer && !bt_addr_le_eq(info->addr,&remembered)) return;
    bool match=false; bt_data_parse(buffer,advertisement,&match);
    if (!match || !atomic_cas(&connecting,0,1)) return;
    bt_addr_le_copy(&found_address,info->addr);
    k_work_submit(&connect_work);
}
static struct bt_le_scan_cb scan_callbacks={.recv=scan_received};
static void retry_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (source) {
        if (atomic_get(&link_ready) && has_peer) settings_save_one("keypoint_relay/peer",&remembered,sizeof(remembered));
        return;
    }
    if (scanning || atomic_get(&connecting)) return;
    /* Scan only while disconnected.  A 10 ms window each second can miss
     * many advertisements; cover 100 ms each 250 ms without competing with
     * the HID connection once it is established. Units are 0.625 ms. */
    const struct bt_le_scan_param parameters={.type=BT_LE_SCAN_TYPE_PASSIVE,.interval=400,.window=160};
    if (!bt_le_scan_start(&parameters,NULL)) scanning=true;
    else k_work_reschedule(&retry_work,K_SECONDS(1));
}
static void connect_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    bt_le_scan_stop(); scanning=false;
    int ret=bt_conn_le_create(&found_address,BT_CONN_LE_CREATE_CONN,BT_LE_CONN_PARAM(6,6,0,100),&source);
    if (ret) { source=NULL; atomic_clear(&connecting); k_work_reschedule(&retry_work,K_SECONDS(1)); }
}
static void connected(struct bt_conn *conn,uint8_t error) {
    if (conn!=source) return;
    atomic_clear(&connecting);
    if (error) { bt_conn_unref(source); source=NULL; k_work_reschedule(&retry_work,K_SECONDS(1)); return; }
    bt_conn_set_security(conn,BT_SECURITY_L2);
    k_work_reschedule(&discover_work,K_MSEC(100));
}
static void security_changed(struct bt_conn *conn,bt_security_t level,enum bt_security_err error) {
    if (conn!=source) return;
    if (error) { bt_conn_disconnect(conn,BT_HCI_ERR_AUTH_FAIL); return; }
    if (level>=BT_SECURITY_L2) k_work_reschedule(&discover_work,K_MSEC(25));
}
static void disconnected(struct bt_conn *conn,uint8_t reason) {
    ARG_UNUSED(reason);
    if (conn!=source) return;
    atomic_clear(&link_ready); atomic_clear(&connecting); discovering=false;
    k_spinlock_key_t key=k_spin_lock(&source_lock);
    struct bt_conn *old=source;source=NULL;
    k_spin_unlock(&source_lock,key);
    bt_conn_unref(old);
    k_work_cancel_delayable(&discover_work);
    k_msgq_purge(&packets); atomic_set(&clear_pending,1); k_work_submit(&receive_work);
    k_work_reschedule(&retry_work,K_MSEC(100));
}
BT_CONN_CB_DEFINE(relay_receiver_connections)={.connected=connected,.disconnected=disconnected,.security_changed=security_changed};
static int indicators_changed(const zmk_event_t *event) {
    const struct zmk_hid_indicators_changed *change=as_zmk_hid_indicators_changed(event);
    if (change && source && atomic_get(&link_ready)) {
        uint8_t value[]={0x81,change->indicators};
        bt_gatt_write_without_response(source,control_handle,value,sizeof(value),false);
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(relay_indicators,indicators_changed);
ZMK_SUBSCRIPTION(relay_indicators,zmk_hid_indicators_changed);
void zmk_keypoint_relay_start(void) {
    bt_le_scan_cb_register(&scan_callbacks);
    k_work_reschedule(&retry_work,K_NO_WAIT);
}
