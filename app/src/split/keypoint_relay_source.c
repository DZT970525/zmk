/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/settings/settings.h>
#include <zmk/usb.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/keypoint_relay.h>
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/keypoint_quota.h>
#include <zmk/hid.h>
#include <zmk/hid_indicators.h>
#include <zmk/endpoints.h>
#include <zmk/battery.h>
#include <zmk/wpm.h>
#include <zmk/keymap.h>
#include <zmk/split/central.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/events/battery_state_changed.h>

ZMK_EVENT_IMPL(zmk_keypoint_relay_changed);
BUILD_ASSERT(sizeof(struct zmk_hid_keyboard_report_body) <= 19);
BUILD_ASSERT(sizeof(struct zmk_hid_consumer_report_body) <= 19);
BUILD_ASSERT(sizeof(struct zmk_hid_mouse_report_body) <= 19);
static struct bt_le_ext_adv *advertiser;
static bool advertiser_slow;
static struct bt_conn *receiver;
static atomic_t enabled, tx_fault, last_buttons;
static atomic_t status_invalid = ATOMIC_INIT(1);
static bool previous_route;
static bool previous_usb_ready;
static atomic_t manual_switch_enabled, route_override;
enum { ROUTE_AUTO, ROUTE_LOCAL, ROUTE_RECEIVER };
static struct k_spinlock connection_lock;
static struct k_work_q tx_queue;
K_THREAD_STACK_DEFINE(tx_stack, 2048);
K_MSGQ_DEFINE(packets, sizeof(struct keypoint_relay_packet), 64, 4);
static void advertise(struct k_work *work);
static void transmit(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(advertise_work, advertise);
K_WORK_DEFINE(tx_work, transmit);
static void status_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(status_work, status_work_handler);
static void connection_event(struct k_work *work) {
    raise_zmk_keypoint_relay_changed((struct zmk_keypoint_relay_changed){.connected=atomic_get(&enabled)});
}
K_WORK_DEFINE(connection_work, connection_event);
bool zmk_keypoint_relay_is_connection(struct bt_conn *conn) {
    struct bt_conn_info info;
    return conn && !bt_conn_get_info(conn,&info) && info.id==1 && info.role==BT_CONN_ROLE_PERIPHERAL;
}
bool zmk_keypoint_relay_connected(void) { return atomic_get(&enabled); }
bool zmk_keypoint_relay_routed(void) {
    int route = atomic_get(&route_override);
    return zmk_keypoint_relay_connected() && route != ROUTE_LOCAL &&
        (route == ROUTE_RECEIVER || zmk_endpoint_get_selected().transport != ZMK_TRANSPORT_USB);
}
static void route_changed(void) {
    k_work_submit(&connection_work);
    k_work_reschedule_for_queue(&tx_queue, &status_work, K_NO_WAIT);
}
bool zmk_keypoint_relay_manual_enabled(void) { return atomic_get(&manual_switch_enabled); }
int zmk_keypoint_relay_toggle_route(void) {
    if (!zmk_keypoint_relay_manual_enabled()) return -EACCES;
    if (!zmk_keypoint_relay_connected()) return -ENOTCONN;
    bool routed = zmk_keypoint_relay_routed();
    zmk_endpoint_clear_reports();
    atomic_set(&route_override, routed ? ROUTE_LOCAL : ROUTE_RECEIVER);
    route_changed();
    return 0;
}
int zmk_keypoint_relay_set_manual_enabled(bool enabled_value) {
    uint8_t value = enabled_value;
    if (value == !!atomic_get(&manual_switch_enabled)) return 0;
    int ret = settings_save_one("keypoint_role/manual_switch", &value, sizeof(value));
    if (ret) return ret;
    atomic_set(&manual_switch_enabled, value);
    if (!value && atomic_get(&route_override) != ROUTE_AUTO) {
        zmk_endpoint_clear_reports();
        atomic_set(&route_override, ROUTE_AUTO);
        route_changed();
    }
    return 0;
}
static int relay_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, "manual_switch")) return -ENOENT;
    if (len != 1) return -EINVAL;
    uint8_t value;
    int ret = read_cb(cb_arg, &value, sizeof(value));
    if (ret < 0) return ret;
    if (ret != 1 || value > 1) return -EINVAL;
    atomic_set(&manual_switch_enabled, value);
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(keypoint_relay_role, "keypoint_role", NULL, relay_settings_set, NULL, NULL);
static struct bt_conn *get_receiver(void) {
    k_spinlock_key_t key=k_spin_lock(&connection_lock);
    struct bt_conn *conn=receiver ? bt_conn_ref(receiver) : NULL;
    k_spin_unlock(&connection_lock,key);
    return conn;
}
static ssize_t control(struct bt_conn *conn,const struct bt_gatt_attr *attr,
                       const void *buffer,uint16_t length,uint16_t offset,uint8_t flags) {
    ARG_UNUSED(attr); ARG_UNUSED(flags);
    if (!zmk_keypoint_relay_is_connection(conn)) return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    const uint8_t *bytes=buffer;
    if (offset || !length) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    if (bytes[0]==0x80 && length==5 && bytes[1]==1 &&
        bytes[2]==sizeof(struct zmk_hid_keyboard_report_body) &&
        bytes[3]==sizeof(struct zmk_hid_consumer_report_body) &&
        bytes[4]==sizeof(struct zmk_hid_mouse_report_body)) {
        return length;
    }
    if (bytes[0]==0x81 && length==2) {
        if (zmk_keypoint_relay_routed())
            zmk_hid_indicators_set_profile(bytes[1],zmk_endpoint_get_selected());
        return length;
    }
    if(bytes[0]==0x82 && length==13) {
        int ret=zmk_keypoint_quota_relay_receive(bytes+1,12);
        return ret ? BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION) : length;
    }
    if(bytes[0]==0x83 && length==1) {
        if(zmk_keypoint_relay_routed())zmk_keypoint_quota_clear();
        return length;
    }
    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
}
static void subscription_changed(const struct bt_gatt_attr *attr,uint16_t value) {
    ARG_UNUSED(attr);
    struct bt_conn *conn=get_receiver();
    atomic_set(&enabled, value==BT_GATT_CCC_NOTIFY && conn && bt_conn_get_security(conn)>=BT_SECURITY_L2);
    if (conn) bt_conn_unref(conn);
    k_work_submit(&connection_work);
    k_work_reschedule_for_queue(&tx_queue,&status_work,K_NO_WAIT);
}
BT_GATT_SERVICE_DEFINE(relay_service,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(KEYPOINT_RELAY_SERVICE)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(KEYPOINT_RELAY_REPORT),BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE,NULL,NULL,NULL),
    BT_GATT_CCC(subscription_changed,BT_GATT_PERM_READ_ENCRYPT|BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(KEYPOINT_RELAY_CONTROL),BT_GATT_CHRC_WRITE|BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE_ENCRYPT,NULL,control,NULL));
int zmk_keypoint_relay_send(uint8_t id,const void *body,size_t length) {
    if (!atomic_get(&enabled)) return -ENOTCONN;
    if (length>19) return -EMSGSIZE;
    /* Preserve a reserve for releases/keyboard reports during fast pointing. */
    if (id==3 && k_msgq_num_free_get(&packets)<8 &&
        *(const uint8_t *)body==atomic_get(&last_buttons)) return -ENOBUFS;
    struct keypoint_relay_packet packet={.length=length+1,.data={id}};
    memcpy(packet.data+1,body,length);
    int ret=k_msgq_put(&packets,&packet,K_NO_WAIT);
    if (!ret) {
        if (id==3) atomic_set(&last_buttons,*(const uint8_t *)body);
    } else atomic_set(&tx_fault,1);
    k_work_submit_to_queue(&tx_queue,&tx_work);
    return ret;
}
static void transmit(struct k_work *work) {
    ARG_UNUSED(work);
    if (atomic_set(&tx_fault,0)) {
        struct bt_conn *conn=get_receiver();
        if (conn) { bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN); bt_conn_unref(conn); }
        k_msgq_purge(&packets); return;
    }
    struct keypoint_relay_packet packet;
    for (int batch = 0; batch < 8 && !k_msgq_get(&packets,&packet,K_NO_WAIT); batch++) {
        struct bt_conn *conn=get_receiver();
        if (!conn) { k_msgq_purge(&packets); return; }
        int ret=bt_gatt_notify(conn,&relay_service.attrs[2],packet.data,packet.length);
        bt_conn_unref(conn);
        /* Never send a later key release ahead of a stalled key press. */
        if (ret) {
            conn=get_receiver();
            if (conn) { bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN); bt_conn_unref(conn); }
            k_msgq_purge(&packets);
            return;
        }
    }
    if (k_msgq_num_used_get(&packets)) k_work_submit_to_queue(&tx_queue,&tx_work);
}
static void status_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    static uint8_t last_payload[19];
    static bool last_payload_valid;
    if (atomic_set(&status_invalid, 0)) last_payload_valid = false;
    struct bt_conn *conn=get_receiver();
    bool subscribed=conn && bt_gatt_is_subscribed(conn,&relay_service.attrs[2],BT_GATT_CCC_NOTIFY);
    if (conn) bt_conn_unref(conn);
    if (subscribed!=!!atomic_get(&enabled)) {
        atomic_set(&enabled,subscribed); k_work_submit(&connection_work);
    }
    if (!subscribed) last_payload_valid = false;
    bool routed=zmk_keypoint_relay_routed();
    if (atomic_get(&enabled) && !routed && previous_route) {
        struct zmk_hid_keyboard_report_body keyboard={0};
        struct zmk_hid_consumer_report_body consumer={0};
        struct zmk_hid_mouse_report_body mouse={0};
        zmk_keypoint_relay_send(1,&keyboard,sizeof(keyboard));
        zmk_keypoint_relay_send(2,&consumer,sizeof(consumer));
        zmk_keypoint_relay_send(3,&mouse,sizeof(mouse));
    }
    previous_route=routed;
    if (atomic_get(&enabled)) {
        uint8_t payload[19]={0};
        payload[0]=zmk_battery_state_of_charge();
        uint8_t right=0;
        if (zmk_split_central_peripherals_connected()) zmk_split_central_get_peripheral_battery_level(0,&right);
        payload[1]=right;
        if (routed) {
        payload[2]=MIN(zmk_wpm_get_state(),255);
        payload[3]=zmk_keymap_highest_layer_active();
        payload[4]=zmk_hid_get_keyboard_report()->body.modifiers;
        const char *name=zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(payload[3]));
        if (name) strncpy((char *)payload+5,name,14);
        } else if (last_payload_valid) {
            /* Local input must not animate the receiver's status display. */
            memcpy(payload + 2, last_payload + 2, 17);
            payload[4] = 0;
        }
        if (!last_payload_valid || memcmp(last_payload, payload, sizeof(payload))) {
            if (!zmk_keypoint_relay_send(4,payload,sizeof(payload))) {
                memcpy(last_payload, payload, sizeof(payload));
                last_payload_valid = true;
            }
        }
    }
    k_work_reschedule_for_queue(&tx_queue,&status_work,K_MSEC(500));
}
static void advertise(struct k_work *work) {
    ARG_UNUSED(work);
    struct bt_conn *conn=get_receiver();
    if (conn) { bt_conn_unref(conn); return; }
    if (!advertiser) return;
#if IS_ENABLED(CONFIG_ZMK_KEYPOINT_NO_IDLE_RELAY_ADVERTISING)
    bt_le_ext_adv_stop(advertiser);
    return;
#endif
    /* Keep hotplug discovery, but stop competing with active host HID traffic. */
    bool slow = zmk_ble_active_profile_is_connected() || zmk_usb_is_hid_ready();
    if (slow != advertiser_slow) {
        const struct bt_le_adv_param parameters = {
            .id=1, .options=BT_LE_ADV_OPT_CONN|BT_LE_ADV_OPT_EXT_ADV,
            .interval_min=slow ? BT_GAP_ADV_SLOW_INT_MIN : BT_GAP_ADV_FAST_INT_MIN_2,
            .interval_max=slow ? BT_GAP_ADV_SLOW_INT_MAX : BT_GAP_ADV_FAST_INT_MAX_2,
        };
        int ret=bt_le_ext_adv_stop(advertiser);
        if (ret && ret!=-EALREADY) {
            k_work_reschedule(&advertise_work,K_MSEC(100)); return;
        }
        ret=bt_le_ext_adv_update_param(advertiser,&parameters);
        if (ret) { k_work_reschedule(&advertise_work,K_MSEC(100)); return; }
        advertiser_slow=slow;
    }
    int ret=bt_le_ext_adv_start(advertiser,BT_LE_EXT_ADV_START_DEFAULT);
    if (ret && ret!=-EALREADY) k_work_reschedule(&advertise_work,K_SECONDS(1));
}
static void connected(struct bt_conn *conn,uint8_t error) {
    if (error || !zmk_keypoint_relay_is_connection(conn)) return;
    k_spinlock_key_t key=k_spin_lock(&connection_lock);
    receiver=bt_conn_ref(conn);
    k_spin_unlock(&connection_lock,key);
    bt_conn_set_security(conn,BT_SECURITY_L2);
    bt_conn_le_param_update(conn,BT_LE_CONN_PARAM(6,6,0,100));
}
static void disconnected(struct bt_conn *conn,uint8_t reason) {
    ARG_UNUSED(reason);
    if (!zmk_keypoint_relay_is_connection(conn)) return;
    atomic_clear(&enabled);
    atomic_set(&route_override, ROUTE_AUTO);
    atomic_set(&status_invalid, 1);
    k_spinlock_key_t key=k_spin_lock(&connection_lock);
    struct bt_conn *old=receiver;
    receiver=NULL;
    k_spin_unlock(&connection_lock,key);
    if (old) bt_conn_unref(old);
    k_msgq_purge(&packets);
    k_work_submit(&connection_work);
    k_work_reschedule(&advertise_work,K_MSEC(100));
}
BT_CONN_CB_DEFINE(relay_source_connections)={.connected=connected,.disconnected=disconnected};
static int relay_layer_changed(const zmk_event_t *event) {
    if (as_zmk_ble_active_profile_changed(event) || as_zmk_usb_conn_state_changed(event))
        k_work_reschedule(&advertise_work,K_NO_WAIT);
    if (as_zmk_usb_conn_state_changed(event)) {
        bool ready = zmk_usb_is_hid_ready();
        if (ready != previous_usb_ready) {
            previous_usb_ready = ready;
            atomic_set(&route_override, ROUTE_AUTO);
            k_work_submit(&connection_work);
        }
    }
    if (atomic_get(&enabled)) k_work_reschedule_for_queue(&tx_queue,&status_work,K_NO_WAIT);
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(relay_layer_status, relay_layer_changed);
ZMK_SUBSCRIPTION(relay_layer_status,zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(relay_layer_status, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(relay_layer_status, zmk_wpm_state_changed);
ZMK_SUBSCRIPTION(relay_layer_status, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(relay_layer_status, zmk_peripheral_battery_state_changed);
ZMK_SUBSCRIPTION(relay_layer_status, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(relay_layer_status, zmk_usb_conn_state_changed);
void zmk_keypoint_relay_start(void) {
    previous_usb_ready = zmk_usb_is_hid_ready();
    k_work_queue_start(&tx_queue,tx_stack,K_THREAD_STACK_SIZEOF(tx_stack),5,NULL);
    size_t count=0; bt_id_get(NULL,&count);
    if (count<2 && bt_id_create(NULL,NULL)!=1) return;
    const struct bt_le_adv_param parameters={.id=1,.options=BT_LE_ADV_OPT_CONN|BT_LE_ADV_OPT_EXT_ADV,
        .interval_min=BT_GAP_ADV_FAST_INT_MIN_2,.interval_max=BT_GAP_ADV_FAST_INT_MAX_2};
    if (bt_le_ext_adv_create(&parameters,NULL,&advertiser)) return;
    const struct bt_data ad[]={
        BT_DATA_BYTES(BT_DATA_FLAGS,BT_LE_AD_GENERAL|BT_LE_AD_NO_BREDR),
        BT_DATA_BYTES(BT_DATA_UUID128_ALL,KEYPOINT_RELAY_SERVICE)};
    if (bt_le_ext_adv_set_data(advertiser,ad,ARRAY_SIZE(ad),NULL,0)) return;
    k_work_reschedule(&advertise_work,K_NO_WAIT);
    k_work_reschedule_for_queue(&tx_queue,&status_work,K_SECONDS(1));
}
