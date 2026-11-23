#include <zephyr/sys/byteorder.h>
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

struct bond_lookup { const bt_addr_le_t *address; bool found; };
static void find_pair_bond(const struct bt_bond_info *info, void *context) {
    struct bond_lookup *lookup=context;
    if (bt_addr_le_eq(&info->addr,lookup->address)) lookup->found=true;
}
static bool pair_bond_exists(uint8_t id,const bt_addr_le_t *address) {
    struct bond_lookup lookup={.address=address};
    bt_foreach_bond(id,find_pair_bond,&lookup);
    return lookup.found;
}

ZMK_EVENT_IMPL(zmk_keypoint_relay_changed);
BUILD_ASSERT(sizeof(struct zmk_hid_keyboard_report_body) <= 19);
BUILD_ASSERT(sizeof(struct zmk_hid_consumer_report_body) <= 19);
BUILD_ASSERT(sizeof(struct zmk_hid_mouse_report_body) <= 19);
static struct bt_le_ext_adv *advertiser;
static bool advertiser_slow;
static struct bt_conn *receiver;
static atomic_t offered, approved, consumed_spaces;
static uint32_t offer_nonce;
static bt_addr_le_t confirmed_peer;
static bool have_confirmed_peer;
static struct bt_conn *get_receiver(void);
static bool report_subscribed(struct bt_conn *conn);
static void confirm_work_handler(struct k_work *work);
static void pair_timeout_handler(struct k_work *work);
static void pair_save_handler(struct k_work *work);
K_WORK_DEFINE(confirm_work, confirm_work_handler);
K_WORK_DEFINE(pair_save_work, pair_save_handler);
K_WORK_DELAYABLE_DEFINE(pair_timeout, pair_timeout_handler);
bool zmk_mac_pair_pending(void) { return atomic_get(&offered) && !atomic_get(&approved); }
bool zmk_mac_pair_allowed(struct bt_conn *conn) {
    return zmk_keypoint_relay_is_connection(conn) && atomic_get(&offered) && atomic_get(&approved);
}

static atomic_t enabled, tx_fault, last_buttons, receiver_ready, ccc_ready, tx_epoch;
static atomic_t diag_notify_errors,diag_blocked_ms;
static void source_diagnostics(struct k_work *work);
K_WORK_DEFINE(source_diag_work,source_diagnostics);
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
static void retry_tx(struct k_work *work) { ARG_UNUSED(work); k_work_submit_to_queue(&tx_queue,&tx_work); }
K_WORK_DELAYABLE_DEFINE(tx_retry,retry_tx);
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
bool zmk_mac_pair_connecting(void) { return atomic_get(&offered) && atomic_get(&approved) && !atomic_get(&enabled); }
bool zmk_keypoint_relay_routed(void) {
    int route = atomic_get(&route_override);
    return zmk_keypoint_relay_connected() && route != ROUTE_LOCAL;
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
/* Keep a locally routed keyboard from servicing an unused 7.5 ms relay link.
 * Run on the status queue; change parameters only on route changes or bounded retries. */
static atomic_t relay_param_reset;
static void tune_receiver_link(struct bt_conn *conn) {
    static int requested_mode = -1;
    static unsigned attempts;
    static int64_t retry_after;
    if (atomic_set(&relay_param_reset, 0)) { requested_mode=-1; attempts=0; retry_after=0; }
    int mode = atomic_get(&enabled) && !zmk_keypoint_relay_routed() ? 1 : 0;
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info)) return;
    bool matches = mode ? (info.le.interval>=24 && info.le.interval<=40 && info.le.latency==4)
                        : (info.le.interval==KEYPOINT_RELAY_ACTIVE_INTERVAL && info.le.latency==0);
    if (matches) { requested_mode=mode; attempts=0; return; }
    int64_t now=k_uptime_get();
    if (mode!=requested_mode) { requested_mode=mode; attempts=0; retry_after=0; }
    if (attempts>=3 || now<retry_after) return;
    const struct bt_le_conn_param *param = mode ? BT_LE_CONN_PARAM(24,40,4,400)
                                              : BT_LE_CONN_PARAM(KEYPOINT_RELAY_ACTIVE_INTERVAL,KEYPOINT_RELAY_ACTIVE_INTERVAL,0,400);
    int ret=bt_conn_le_param_update(conn,param);
    attempts++; retry_after=now+5000;
    ARG_UNUSED(ret);
}
static void pair_save_handler(struct k_work *work) {
    ARG_UNUSED(work);
    struct bt_conn *conn = get_receiver();
    if (!conn) return;
    if (atomic_get(&enabled) && zmk_mac_pair_allowed(conn) && bt_conn_get_security(conn) >= BT_SECURITY_L2) {
        bt_addr_le_copy(&confirmed_peer, bt_conn_get_dst(conn));
        have_confirmed_peer = true;
        settings_save_one("mac_test/peer", &confirmed_peer, sizeof(confirmed_peer));
    }
    bt_conn_unref(conn);
}
static int pair_settings_set(const char *name, size_t length, settings_read_cb read, void *arg) {
    if (strcmp(name,"peer") || length != sizeof(confirmed_peer)) return -ENOENT;
    int rc = read(arg, &confirmed_peer, sizeof(confirmed_peer));
    if (rc != sizeof(confirmed_peer)) return -EINVAL;
    have_confirmed_peer = true; return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(mac_test, "mac_test", NULL, pair_settings_set, NULL, NULL);
static void pair_timeout_handler(struct k_work *work) {
    ARG_UNUSED(work);
    struct bt_conn *conn=get_receiver();
    if (conn) {
        if (!atomic_get(&enabled)) bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(conn);
    }
}
static void confirm_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    struct bt_conn *conn=get_receiver();
    if (!conn) return;
    if (zmk_mac_pair_allowed(conn)) {
        /* Retain explicit consent in RAM across a handshake reconnect. Persist only after READY. */
        bt_addr_le_copy(&confirmed_peer,bt_conn_get_dst(conn));have_confirmed_peer=true;
        zmk_endpoint_clear_reports();
        atomic_set(&route_override, ROUTE_RECEIVER);
        /* Only the receiver initiates SMP, after observing approval. */
        k_work_reschedule(&pair_timeout,K_SECONDS(30));
    }
    bt_conn_unref(conn);
    k_work_submit(&connection_work);
}
/* This gate runs before keymap dispatch, including persisted Studio maps.
 * B is position 45 in the unified 66-key SOFLE layout. Keep press/release
 * paired even if the layer or permission changes while B is held. */
static bool route_key_held;
bool zmk_mac_pair_key(uint32_t position, bool pressed) {
    if (position == (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) ? 31 : 45)) {
        if (route_key_held) {
            if (!pressed) route_key_held=false;
            return true;
        }
        if (pressed && zmk_keypoint_relay_manual_enabled() &&
            zmk_keymap_highest_layer_active() == 1) {
            route_key_held=true;
            zmk_keypoint_relay_toggle_route();
            return true;
        }
    }
    int bit = position == (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) ? 47 : 59) ? 0 : position == (IS_ENABLED(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT) ? 50 : 62) ? 1 : -1;
    if (bit < 0) return false;
    if (!pressed && atomic_test_and_clear_bit(&consumed_spaces,bit)) return true;
    if (pressed && (atomic_get(&consumed_spaces) & (1u << bit))) return true;
    if (!pressed || !zmk_mac_pair_pending()) return false;
    atomic_set_bit(&consumed_spaces,bit);
    if (atomic_cas(&approved,0,1)) k_work_submit(&confirm_work);
    return true;
}
static ssize_t control_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset) {
    if (!zmk_keypoint_relay_is_connection(conn)) return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    #if IS_ENABLED(CONFIG_BOARD_ZITAOTECH_KEYPOINT_LEFT)
    uint8_t reply[7]={1, atomic_get(&approved) ? 2 : atomic_get(&offered) ? 1 : 0};
    reply[6]=2; /* Version1 optional product field; legacy six-byte SOFLE remains supported. */
#else
    uint8_t reply[6]={1, atomic_get(&approved) ? 2 : atomic_get(&offered) ? 1 : 0};
#endif
    sys_put_le32(offer_nonce,reply+2);
    return bt_gatt_attr_read(conn,attr,buf,len,offset,reply,sizeof(reply));
}

static ssize_t control(struct bt_conn *conn,const struct bt_gatt_attr *attr,
                       const void *buffer,uint16_t length,uint16_t offset,uint8_t flags) {
    ARG_UNUSED(attr); ARG_UNUSED(flags);
    if (!zmk_keypoint_relay_is_connection(conn)) return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    const uint8_t *bytes=buffer;
    if (offset || !length) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    if (bytes[0]==0x90 && length==6 && bytes[1]==1) {
        uint32_t nonce=sys_get_le32(bytes+2);
        if (atomic_get(&offered))
            return nonce==offer_nonce ? length : BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        offer_nonce=nonce; atomic_set(&offered,1);
        /* Identity 1 contains only explicitly approved receiver bonds.
         * Do not restrict reconnects to one separately remembered address. */
        if (pair_bond_exists(1,bt_conn_get_dst(conn))) {
            atomic_set(&approved,1); k_work_submit(&confirm_work);
        }
        k_work_submit(&connection_work);
        return length;
    }
    if (!zmk_mac_pair_allowed(conn) || bt_conn_get_security(conn)<BT_SECURITY_L2)
        return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    if (bytes[0]==0x91 && length==6 && bytes[1]==1 && sys_get_le32(bytes+2)==offer_nonce) {
        /* A restored CCC does not necessarily emit cfg_changed again. */
        if (!report_subscribed(conn)) return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
        atomic_set(&receiver_ready,1); atomic_set(&enabled,1);
        k_work_cancel_delayable(&pair_timeout); k_work_submit(&pair_save_work);
        k_work_submit(&connection_work);atomic_set(&status_invalid,1);
        k_work_reschedule_for_queue(&tx_queue,&status_work,K_MSEC(100));
        return length;
    }
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
    if(bytes[0]==0x84 && length==1) {
        k_work_submit_to_queue(&tx_queue,&source_diag_work);return length;
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
    bool subscribed=value==BT_GATT_CCC_NOTIFY && conn && zmk_mac_pair_allowed(conn) && bt_conn_get_security(conn)>=BT_SECURITY_L2;
    atomic_set(&ccc_ready,subscribed);
    if (conn) bt_conn_unref(conn);
    if (!subscribed) { atomic_clear(&enabled);atomic_clear(&receiver_ready); }
    k_work_submit(&connection_work);

}
BT_GATT_SERVICE_DEFINE(relay_service,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(KEYPOINT_RELAY_SERVICE)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(KEYPOINT_RELAY_REPORT),BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE,NULL,NULL,NULL),
    BT_GATT_CCC(subscription_changed,BT_GATT_PERM_READ_ENCRYPT|BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(KEYPOINT_RELAY_CONTROL),BT_GATT_CHRC_READ|BT_GATT_CHRC_WRITE|BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_READ|BT_GATT_PERM_WRITE,control_read,control,NULL));
static bool report_subscribed(struct bt_conn *conn) {
    return conn && bt_gatt_is_subscribed(conn,&relay_service.attrs[2],BT_GATT_CCC_NOTIFY);
}
static struct k_spinlock report_cache_lock;
static struct zmk_hid_keyboard_report_body cached_keyboard;
static struct zmk_hid_consumer_report_body cached_consumer;
static zmk_mouse_button_flags_t cached_mouse_buttons;
BUILD_ASSERT(sizeof(cached_keyboard)+1<=19,"Keyboard snapshot must fit relay packet");
int zmk_keypoint_relay_send(uint8_t id,const void *body,size_t length) {
    if (!atomic_get(&enabled)) return -ENOTCONN;
    if (length>19) return -EMSGSIZE;
    /* Preserve a reserve for releases/keyboard reports during fast pointing. */
    if (id==3 && k_msgq_num_free_get(&packets)<8 &&
        *(const uint8_t *)body==atomic_get(&last_buttons)) return -ENOBUFS;
    struct keypoint_relay_packet packet={.length=length+1,.data={id}};
    memcpy(packet.data+1,body,length);
    k_spinlock_key_t cache_key=k_spin_lock(&report_cache_lock);
    int ret=k_msgq_put(&packets,&packet,K_NO_WAIT);
    if (!ret) {
        if(id==1 && length==sizeof(cached_keyboard))memcpy(&cached_keyboard,body,length);
        if(id==2 && length==sizeof(cached_consumer))memcpy(&cached_consumer,body,length);
        if(id==3 && length==sizeof(struct zmk_hid_mouse_report_body))cached_mouse_buttons=((const struct zmk_hid_mouse_report_body *)body)->buttons;
    }
    k_spin_unlock(&report_cache_lock,cache_key);
    if (!ret) {
        if (id==3) atomic_set(&last_buttons,*(const uint8_t *)body);
    } else atomic_set(&tx_fault,1);
    k_work_submit_to_queue(&tx_queue,&tx_work);
    return ret;
}
/* Manual snapshot only: no periodic radio diagnostic traffic or settings writes. */
static void source_diagnostics(struct k_work *work) {
    ARG_UNUSED(work);
    uint8_t data[19]={1},split[12];
    zmk_split_central_link_diagnostics(split);
    memcpy(data+1,split+2,4); /* right interval and latency */
    memcpy(data+5,split+8,4); /* low16 input and failed enqueue counters */
    struct bt_conn *host=zmk_ble_active_profile_conn();
    if(host){struct bt_conn_info info;if(!bt_conn_get_info(host,&info) && info.state==BT_CONN_STATE_CONNECTED){
        sys_put_le16(info.le.interval,data+9);sys_put_le16(info.le.latency,data+11);data[18]|=1;
    }bt_conn_unref(host);}
    sys_put_le16(atomic_get(&diag_notify_errors),data+13);
    sys_put_le16(MIN((uint32_t)atomic_get(&diag_blocked_ms),65535U),data+15);
    data[17]=k_msgq_num_used_get(&packets);if(zmk_keypoint_relay_routed())data[18]|=2;
    zmk_keypoint_relay_send(10,data,sizeof(data));
}
/* Full state refresh, serialized with key transitions; never replay relative deltas. */
static void refresh_relay_state(void) {
    if(!atomic_get(&enabled))return;
    k_spinlock_key_t key=k_spin_lock(&report_cache_lock);
    bool routed=zmk_keypoint_relay_routed();
    struct keypoint_relay_packet snapshot={.length=sizeof(cached_keyboard)+2,.data={7,routed?1:0}};
    if(routed)memcpy(snapshot.data+2,&cached_keyboard,sizeof(cached_keyboard));
    struct zmk_hid_consumer_report_body consumer={0};
    struct zmk_hid_mouse_report_body mouse={0};
    if(routed){consumer=cached_consumer;mouse.buttons=cached_mouse_buttons;}
    struct keypoint_relay_packet c={.length=sizeof(consumer)+1,.data={8}},m={.length=sizeof(mouse)+1,.data={9}};
    memcpy(c.data+1,&consumer,sizeof(consumer));memcpy(m.data+1,&mouse,sizeof(mouse));
    if(k_msgq_put(&packets,&snapshot,K_NO_WAIT) || k_msgq_put(&packets,&c,K_NO_WAIT) || k_msgq_put(&packets,&m,K_NO_WAIT))atomic_set(&tx_fault,1);
    k_spin_unlock(&report_cache_lock,key);
    k_work_submit_to_queue(&tx_queue,&tx_work);
}
/* Merge only adjacent relative motion. Never cross a button, wheel or other
 * report boundary, and leave overflowing reports queued rather than losing distance. */
static bool merge_mouse_motion(struct keypoint_relay_packet *first,
                               const struct keypoint_relay_packet *next) {
    if(first->length!=sizeof(struct zmk_hid_mouse_report_body)+1 ||
       next->length!=first->length || first->data[0]!=3 || next->data[0]!=3)return false;
    struct zmk_hid_mouse_report_body a,b;
    memcpy(&a,first->data+1,sizeof(a));memcpy(&b,next->data+1,sizeof(b));
    if(a.buttons!=b.buttons || a.d_scroll_x || a.d_scroll_y || b.d_scroll_x || b.d_scroll_y ||
       (!a.d_x && !a.d_y) || (!b.d_x && !b.d_y))return false;
    int32_t x=(int32_t)a.d_x+b.d_x,y=(int32_t)a.d_y+b.d_y;
    if(x < -32768 || x > 32767 || y < -32768 || y > 32767)return false;
    a.d_x=x;a.d_y=y;memcpy(first->data+1,&a,sizeof(a));return true;
}
static void transmit(struct k_work *work) {
    ARG_UNUSED(work);
    static struct keypoint_relay_packet packet;
    static bool pending;
    static int epoch;
    static int64_t blocked_since;
    static unsigned merged_count;
    int current=atomic_get(&tx_epoch);
    if(current!=epoch){epoch=current;pending=false;blocked_since=0;}
    if (atomic_set(&tx_fault,0)) {
        struct bt_conn *conn=get_receiver();
        if(conn){bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);bt_conn_unref(conn);}
        pending=false;k_msgq_purge(&packets);return;
    }
    for(int batch=0;batch<8;batch++) {
        if(!pending) {
            if(k_msgq_get(&packets,&packet,K_NO_WAIT))break;
            merged_count=0;
        }
        pending=true;
        /* Sole queue consumer: peek before removing so a boundary remains FIFO.
         * Bound work per batch even during a sustained stream of pointer reports. */
        for(;merged_count<7;merged_count++) {
            struct keypoint_relay_packet next;
            if(k_msgq_peek(&packets,&next) || !merge_mouse_motion(&packet,&next))break;
            k_msgq_get(&packets,&next,K_NO_WAIT);
        }
        struct bt_conn *conn=get_receiver();
        if(!conn){pending=false;k_msgq_purge(&packets);return;}
        int ret=bt_gatt_notify(conn,&relay_service.attrs[2],packet.data,packet.length);
        if(ret) {
            atomic_inc(&diag_notify_errors);
            bool transient=ret==-ENOMEM || ret==-EAGAIN || ret==-ENOBUFS;
            if(!blocked_since)blocked_since=k_uptime_get();
            uint32_t blocked=k_uptime_get()-blocked_since;
            if(blocked>(uint32_t)atomic_get(&diag_blocked_ms))atomic_set(&diag_blocked_ms,blocked);
            if(transient && k_uptime_get()-blocked_since<2000) {
                bt_conn_unref(conn);
                k_work_reschedule_for_queue(&tx_queue,&tx_retry,K_MSEC(10));return;
            }
            bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);bt_conn_unref(conn);
            pending=false;blocked_since=0;k_msgq_purge(&packets);return;
        }
        bt_conn_unref(conn);pending=false;blocked_since=0;
    }
    if(k_msgq_num_used_get(&packets))k_work_submit_to_queue(&tx_queue,&tx_work);
}
static void status_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    static uint8_t last_payload[19];
    static bool last_payload_valid;
    if (atomic_set(&status_invalid, 0)) last_payload_valid = false;
    struct bt_conn *conn=get_receiver();
    bool subscribed=atomic_get(&receiver_ready) && conn && zmk_mac_pair_allowed(conn) && bt_conn_get_security(conn)>=BT_SECURITY_L2 && bt_gatt_is_subscribed(conn,&relay_service.attrs[2],BT_GATT_CCC_NOTIFY);
    if (conn) { tune_receiver_link(conn); bt_conn_unref(conn); }
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
    refresh_relay_state();
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
            .id=1, .options=BT_LE_ADV_OPT_CONN|BT_LE_ADV_OPT_EXT_ADV|BT_LE_ADV_OPT_USE_IDENTITY,
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
    k_spinlock_key_t cache_key=k_spin_lock(&report_cache_lock);
    memset(&cached_keyboard,0,sizeof(cached_keyboard));memset(&cached_consumer,0,sizeof(cached_consumer));cached_mouse_buttons=0;
    k_spin_unlock(&report_cache_lock,cache_key);
    atomic_set(&relay_param_reset,1);
    atomic_clear(&offered); atomic_clear(&approved); atomic_clear(&receiver_ready); atomic_clear(&ccc_ready); offer_nonce=0;
    k_work_reschedule(&pair_timeout,K_SECONDS(25));
    bt_conn_le_param_update(conn,BT_LE_CONN_PARAM(KEYPOINT_RELAY_ACTIVE_INTERVAL,KEYPOINT_RELAY_ACTIVE_INTERVAL,0,400));
}
static void disconnected(struct bt_conn *conn,uint8_t reason) {
    ARG_UNUSED(reason);
    if (!zmk_keypoint_relay_is_connection(conn)) return;
    atomic_clear(&enabled); atomic_clear(&offered); atomic_clear(&approved);
    atomic_clear(&receiver_ready);atomic_clear(&ccc_ready);atomic_inc(&tx_epoch);k_work_cancel_delayable(&tx_retry);
    k_work_cancel_delayable(&pair_timeout);
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
static void pair_security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err) {
    if (!zmk_keypoint_relay_is_connection(conn)) return;
    if (err) {
        /* Keep stored bonds; retry the encrypted link without silently unpairing. */
        bt_conn_disconnect(conn,BT_HCI_ERR_AUTH_FAIL);
    }
}
BT_CONN_CB_DEFINE(relay_source_connections)={.connected=connected,.disconnected=disconnected,.security_changed=pair_security_changed};
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
    const struct bt_le_adv_param parameters={.id=1,.options=BT_LE_ADV_OPT_CONN|BT_LE_ADV_OPT_EXT_ADV|BT_LE_ADV_OPT_USE_IDENTITY,
        .interval_min=BT_GAP_ADV_FAST_INT_MIN_2,.interval_max=BT_GAP_ADV_FAST_INT_MAX_2};
    if (bt_le_ext_adv_create(&parameters,NULL,&advertiser)) return;
    const struct bt_data ad[]={
        BT_DATA_BYTES(BT_DATA_FLAGS,BT_LE_AD_GENERAL|BT_LE_AD_NO_BREDR),
        BT_DATA_BYTES(BT_DATA_UUID128_ALL,KEYPOINT_RELAY_SERVICE)};
    if (bt_le_ext_adv_set_data(advertiser,ad,ARRAY_SIZE(ad),NULL,0)) return;
    k_work_reschedule(&advertise_work,K_NO_WAIT);
    k_work_reschedule_for_queue(&tx_queue,&status_work,K_SECONDS(1));
}
