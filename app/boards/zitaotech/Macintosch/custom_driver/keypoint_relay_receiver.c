#include <zephyr/sys/byteorder.h>
#include <zephyr/random/random.h>
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
static atomic_t link_ready, clear_pending, tearing_down;
static struct bt_gatt_discover_params discovery;
static struct bt_gatt_subscribe_params subscription;
static struct bt_gatt_discover_params ccc_discovery;
static struct bt_gatt_write_params hello_write;
static uint16_t report_handle,control_handle;
static const uint8_t hello[]={0x80,1,sizeof(struct zmk_hid_keyboard_report_body),
    sizeof(struct zmk_hid_consumer_report_body),sizeof(struct zmk_hid_mouse_report_body)};
static struct keypoint_relay_status status;
static struct k_spinlock status_lock;
struct receiver_queued_packet { struct keypoint_relay_packet packet; uint32_t at; };
K_MSGQ_DEFINE(packets,sizeof(struct receiver_queued_packet),64,4);
static uint8_t source_diag[19];
static uint32_t source_diag_at;
static bool source_diag_valid;
static struct k_spinlock source_diag_lock;
static atomic_t diag_received,diag_processed,diag_dropped,diag_max_queue_ms;
static void connect_work_handler(struct k_work *work);
static void retry_work_handler(struct k_work *work);
static void receive_work_handler(struct k_work *work);
static void discover_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(retry_work,retry_work_handler);
K_WORK_DELAYABLE_DEFINE(discover_work,discover_work_handler);
K_WORK_DEFINE(connect_work,connect_work_handler);
K_WORK_DEFINE(receive_work,receive_work_handler);
static bt_addr_le_t found_address;
static atomic_t connecting, pair_prompt, read_pending, offer_accepted;
static struct bt_gatt_read_params approval_read;
static struct bt_gatt_write_params offer_write,ready_write;
static uint8_t ready_message[6];
static void send_ready(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(ready_work,send_ready);
static uint8_t offer[6];
static uint32_t nonce;

static bool handshaking;
static uint8_t pending_product;
struct rejected_peer { bt_addr_le_t address; int64_t until; };
static struct rejected_peer rejected[4];
static unsigned rejected_next;
static void approval_poll(struct k_work *work);
static void candidate_expired(struct k_work *work);
static void disconnected(struct bt_conn *conn,uint8_t reason);
static void supervise_link(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(supervise_work,supervise_link);
K_WORK_DELAYABLE_DEFINE(approval_work,approval_poll);
K_WORK_DELAYABLE_DEFINE(candidate_timeout,candidate_expired);
bool zmk_mac_pair_pending(void) { return atomic_get(&pair_prompt); }
static void prompt(bool value) {
    if (atomic_set(&pair_prompt,value) != value)
        raise_zmk_keypoint_relay_changed((struct zmk_keypoint_relay_changed){.connected=atomic_get(&link_ready)});
}
static void reject_current(void) {
    if (!source) return;
    struct rejected_peer *p=&rejected[rejected_next++ % ARRAY_SIZE(rejected)];
    bt_addr_le_copy(&p->address,bt_conn_get_dst(source));p->until=k_uptime_get()+2000;
}
static void candidate_expired(struct k_work *work) {
    ARG_UNUSED(work);
    k_spinlock_key_t key=k_spin_lock(&source_lock);
    struct bt_conn *conn=source && !atomic_get(&link_ready)?bt_conn_ref(source):NULL;
    k_spin_unlock(&source_lock,key);
    if(conn){
        reject_current();bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        /* Disconnect may invoke cleanup immediately: never arm an old timeout
         * against a newly selected keyboard. The ref prevents pointer reuse. */
        if(source==conn && !atomic_get(&link_ready))k_work_reschedule(&candidate_timeout,K_SECONDS(1));
        bt_conn_unref(conn);
    }
}

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
SETTINGS_STATIC_HANDLER_DEFINE(keypoint_relay,"mac_receiver",NULL,settings_set,NULL,NULL);
static atomic_t heartbeat_seen,last_heartbeat;
static void release_lease(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(lease_work,release_lease);
static void release_lease(struct k_work *work) {
    ARG_UNUSED(work);
    if(!atomic_get(&link_ready) || !atomic_get(&heartbeat_seen))return;
    k_spinlock_key_t key=k_spin_lock(&source_lock);
    struct bt_conn *conn=source?bt_conn_ref(source):NULL;
    k_spin_unlock(&source_lock,key);
    if(!conn)return;
    if((uint32_t)(k_uptime_get_32()-(uint32_t)atomic_get(&last_heartbeat))>=2000){
        atomic_clear(&link_ready);atomic_set(&clear_pending,1);k_work_submit(&receive_work);
        bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    } else k_work_reschedule(&lease_work,K_MSEC(500));
    bt_conn_unref(conn);
}
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
    struct receiver_queued_packet queued;
    for(unsigned batch=0;batch<8 && !k_msgq_get(&packets,&queued,K_NO_WAIT);batch++) {
        struct keypoint_relay_packet packet=queued.packet;
        uint32_t age=k_uptime_get_32()-queued.at;
        if (age>(uint32_t)atomic_get(&diag_max_queue_ms)) atomic_set(&diag_max_queue_ms,age);
        atomic_inc(&diag_processed);
        if (!atomic_get(&link_ready)) continue;
        switch (packet.data[0]) {
        case 10:
            if(packet.length==20 && packet.data[1]==1){
                k_spinlock_key_t key=k_spin_lock(&source_diag_lock);
                memcpy(source_diag,packet.data+1,19);source_diag_at=k_uptime_get_32();source_diag_valid=true;
                k_spin_unlock(&source_diag_lock,key);
            }
            break;
        case 7:
            if(packet.length!=sizeof(struct zmk_hid_keyboard_report_body)+2 || packet.data[1]>1)break;
            memcpy(&zmk_hid_get_keyboard_report()->body,packet.data+2,sizeof(struct zmk_hid_keyboard_report_body));
            zmk_endpoint_send_report(0x07);
            if(!packet.data[1]){zmk_endpoint_clear_reports();zmk_endpoint_send_mouse_report();}
            break;

        case 8:
            if(packet.length!=sizeof(struct zmk_hid_consumer_report_body)+1)break;
            memcpy(&zmk_hid_get_consumer_report()->body,packet.data+1,packet.length-1);
            zmk_endpoint_send_report(0x0c);break;
        case 9:
            if(packet.length!=sizeof(struct zmk_hid_mouse_report_body)+1)break;
            memcpy(&zmk_hid_get_mouse_report()->body,packet.data+1,packet.length-1);
            zmk_hid_mouse_movement_set(0,0);zmk_hid_mouse_scroll_set(0,0);
            zmk_endpoint_send_mouse_report();break;
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
    if(k_msgq_num_used_get(&packets))k_work_submit(&receive_work);
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
    if(((const uint8_t *)data)[0]==7 && length==sizeof(struct zmk_hid_keyboard_report_body)+2 && ((const uint8_t *)data)[1]<=1){
        atomic_set(&last_heartbeat,k_uptime_get_32());atomic_set(&heartbeat_seen,1);k_work_reschedule(&lease_work,K_MSEC(500));
    }
    struct receiver_queued_packet queued={.packet={.length=length},.at=k_uptime_get_32()};
    memcpy(queued.packet.data,data,length);
    atomic_inc(&diag_received);
    if (k_msgq_put(&packets,&queued,K_NO_WAIT)) {
        atomic_inc(&diag_dropped);
        /* Loss of an ordered keyboard stream must never leave a held key. */
        atomic_clear(&link_ready); atomic_set(&clear_pending,1);
        bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
    k_work_submit(&receive_work);
    return BT_GATT_ITER_CONTINUE;
}
static void ready_written(struct bt_conn *conn,uint8_t error,struct bt_gatt_write_params *params) {
    ARG_UNUSED(params);
    if(conn!=source || !atomic_get(&offer_accepted) || !handshaking)return;
    if(error){k_work_reschedule(&ready_work,K_MSEC(100));return;}
    k_spinlock_key_t product_key=k_spin_lock(&status_lock);
    status.product=pending_product;
    k_spin_unlock(&status_lock,product_key);
    atomic_set(&link_ready,1);prompt(false);
    raise_zmk_keypoint_relay_changed((struct zmk_keypoint_relay_changed){.connected=true});
    k_work_cancel_delayable(&candidate_timeout);k_work_cancel_delayable(&approval_work);
    zmk_activity_note();bt_addr_le_copy(&remembered,bt_conn_get_dst(conn));has_peer=true;
    k_work_reschedule(&retry_work,K_NO_WAIT);
}
static void send_ready(struct k_work *work) {
    ARG_UNUSED(work);
    if(!source || !atomic_get(&offer_accepted) || !handshaking || !control_handle || atomic_get(&link_ready))return;
    ready_message[0]=0x91;ready_message[1]=1;sys_put_le32(nonce,ready_message+2);
    ready_write=(struct bt_gatt_write_params){.func=ready_written,.handle=control_handle,.data=ready_message,.length=sizeof(ready_message)};
    int rc=bt_gatt_write(source,&ready_write);
    if(rc==-ENOMEM || rc==-EAGAIN || rc==-ENOBUFS)k_work_reschedule(&ready_work,K_MSEC(100));
    else if(rc)bt_conn_disconnect(source,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}
static void subscribed(struct bt_conn *conn,uint8_t error,struct bt_gatt_subscribe_params *params) {
    ARG_UNUSED(params);
    if(conn!=source || !atomic_get(&offer_accepted) || !handshaking)return;
    if(error){bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);return;}
    if(bt_conn_get_security(conn)<BT_SECURITY_L2){bt_conn_disconnect(conn,BT_HCI_ERR_AUTH_FAIL);return;}
    /* Let CCC write completion unwind before the final READY request. */
    k_work_reschedule(&ready_work,K_MSEC(50));
}
static void hello_written(struct bt_conn *conn,uint8_t error,struct bt_gatt_write_params *params) {
    ARG_UNUSED(params);
    if (conn!=source || !atomic_get(&offer_accepted) || !handshaking) return;
    if (error) { bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN); return; }
    memset(&ccc_discovery,0,sizeof(ccc_discovery));
    subscription=(struct bt_gatt_subscribe_params){.notify=notification,.subscribe=subscribed,
        .value=BT_GATT_CCC_NOTIFY,.value_handle=report_handle,.ccc_handle=BT_GATT_AUTO_DISCOVER_CCC_HANDLE,
        .end_handle=UINT16_MAX,.disc_params=&ccc_discovery};
    /* This object is reused for different keyboards. It must leave Zephyr's
     * bonded-client list on disconnect; otherwise assignment corrupts that list. */
    atomic_set_bit(subscription.flags,BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
    atomic_set_bit(subscription.flags,BT_GATT_SUBSCRIBE_FLAG_NO_RESUB);
    int ret=bt_gatt_subscribe(conn,&subscription);
    if(ret==-EALREADY)k_work_reschedule(&ready_work,K_MSEC(50));
    else if(ret)bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}
static uint8_t approval_result(struct bt_conn *conn,uint8_t error,
                              struct bt_gatt_read_params *params,const void *data,uint16_t length) {
    ARG_UNUSED(params);
    if (conn!=source) return BT_GATT_ITER_STOP;
    atomic_clear(&read_pending);
    /* A late polling response must not move READY back to waiting for space. */
    if (!atomic_get(&offer_accepted) || atomic_get(&link_ready) || handshaking) return BT_GATT_ITER_STOP;
    const uint8_t *bytes=data;
    if (error || !data || (length!=6 && length!=7) || (length==7 && (bytes[6]<1 || bytes[6]>2)) || bytes[0]!=1 || sys_get_le32(bytes+2)!=nonce) {
        reject_current();bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);return BT_GATT_ITER_STOP;
    }
    pending_product=length==7 ? bytes[6] : 1;
    if (bytes[1]==1) {
        prompt(true);k_work_reschedule(&approval_work,K_MSEC(200));
    } else if (bytes[1]==2) {
        if (bt_conn_get_security(conn)<BT_SECURITY_L2) {
            int rc=bt_conn_set_security(conn,BT_SECURITY_L2);
            if (rc && rc!=-EALREADY) bt_conn_disconnect(conn,BT_HCI_ERR_AUTH_FAIL);
            else k_work_reschedule(&approval_work,K_MSEC(150));
        } else if (!handshaking) {
            handshaking=true;
            hello_write=(struct bt_gatt_write_params){.func=hello_written,.handle=control_handle,.data=hello,.length=sizeof(hello)};
            if (bt_gatt_write(conn,&hello_write)) bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
    } else {
        k_work_reschedule(&approval_work,K_MSEC(200));
    }
    return BT_GATT_ITER_STOP;
}
static void reset_handshake_state(void) {
    atomic_clear(&offer_accepted);atomic_clear(&read_pending);
    report_handle=0;control_handle=0;nonce=0;pending_product=0;
    discovering=false;handshaking=false;
}
static void approval_poll(struct k_work *work) {
    ARG_UNUSED(work);
    if (!source || !atomic_get(&offer_accepted) || atomic_get(&link_ready) || handshaking || !control_handle) return;
    if (!atomic_cas(&read_pending,0,1)) return;
    approval_read=(struct bt_gatt_read_params){.func=approval_result,.handle_count=1,
        .single={.handle=control_handle,.offset=0}};
    if (bt_gatt_read(source,&approval_read)) {
        atomic_clear(&read_pending);k_work_reschedule(&approval_work,K_MSEC(100));
    }
}
static void offered(struct bt_conn *conn,uint8_t error,struct bt_gatt_write_params *params) {
    ARG_UNUSED(params);
    if (conn!=source) return;
    if (error) { reject_current();bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);return; }
    atomic_set(&offer_accepted,1);
    k_work_reschedule(&approval_work,K_NO_WAIT);
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
    nonce=sys_rand32_get();offer[0]=0x90;offer[1]=1;sys_put_le32(nonce,offer+2);
    offer_write=(struct bt_gatt_write_params){.func=offered,.handle=control_handle,.data=offer,.length=sizeof(offer)};
    /* Wait for the keyboard approval result before asking for user input.
     * A transient address/bond lookup during discovery is not an approval request. */
    prompt(false);
    if (bt_gatt_write(conn,&offer_write)) bt_conn_disconnect(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    return BT_GATT_ITER_STOP;
}
static void discover_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!source || discovering || atomic_get(&link_ready)) return;
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
    if (source || atomic_get(&tearing_down) || !(info->adv_props & BT_GAP_ADV_PROP_CONNECTABLE)) return;
    int64_t now=k_uptime_get();
    for (int i=0;i<ARRAY_SIZE(rejected);i++)
        if (now<rejected[i].until && bt_addr_le_eq(info->addr,&rejected[i].address)) return;
    bool match=false; bt_data_parse(buffer,advertisement,&match);
    if (!match || !atomic_cas(&connecting,0,1)) return;
    bt_addr_le_copy(&found_address,info->addr);
    k_work_submit(&connect_work);
}
static struct bt_le_scan_cb scan_callbacks={.recv=scan_received};
static void retry_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (source) {
        if (atomic_get(&link_ready) && has_peer) settings_save_one("mac_receiver/peer",&remembered,sizeof(remembered));
        return;
    }
    if (atomic_get(&connecting)) return;
    /* Scan continuously only while disconnected. Stop scanning before connecting.
     * Equal interval/window removes discovery gaps. Units are 0.625 ms. */
    const struct bt_le_scan_param parameters={.type=BT_LE_SCAN_TYPE_PASSIVE,.interval=96,.window=96};
    /* Revalidate the controller instead of trusting a stale software flag.
     * EALREADY means the existing scan remains healthy; do not stop/restart it. */
    int err=bt_le_scan_start(&parameters,NULL);
    scanning=!err || err==-EALREADY;
    k_work_reschedule(&retry_work,scanning ? K_SECONDS(2) : K_SECONDS(1));
}
static void connect_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    bt_le_scan_stop(); scanning=false;
    int ret=bt_conn_le_create(&found_address,BT_CONN_LE_CREATE_CONN,BT_LE_CONN_PARAM(KEYPOINT_RELAY_ACTIVE_INTERVAL,KEYPOINT_RELAY_ACTIVE_INTERVAL,0,400),&source);
    if (ret) { source=NULL; atomic_clear(&connecting); k_work_reschedule(&retry_work,K_SECONDS(1)); }
    else { handshaking=false; atomic_clear(&read_pending);k_work_reschedule(&candidate_timeout,K_SECONDS(25)); }
}
static void connected(struct bt_conn *conn,uint8_t error) {
    if (conn!=source) return;
    if(error){disconnected(conn,error);return;}
    atomic_clear(&connecting);
    reset_handshake_state();
    k_work_reschedule(&discover_work,K_MSEC(100));
}
static void security_changed(struct bt_conn *conn,bt_security_t level,enum bt_security_err error) {
    if (conn!=source) return;
    if (error) {
        /* A transient authentication failure must not erase an approved bond. */
        bt_conn_disconnect(conn,BT_HCI_ERR_AUTH_FAIL);return;
    }
    if (level>=BT_SECURITY_L2 && control_handle && atomic_get(&offer_accepted)) k_work_reschedule(&approval_work,K_MSEC(25));
}
static void disconnected(struct bt_conn *conn,uint8_t reason) {
    ARG_UNUSED(reason);
    k_spinlock_key_t key=k_spin_lock(&source_lock);
    if(conn!=source){k_spin_unlock(&source_lock,key);return;}
    atomic_set(&tearing_down,1);
    struct bt_conn *old=source;source=NULL;
    k_spin_unlock(&source_lock,key);
    reset_handshake_state();
    k_spinlock_key_t diag_key=k_spin_lock(&source_diag_lock);source_diag_valid=false;k_spin_unlock(&source_diag_lock,diag_key);
    atomic_clear(&heartbeat_seen);k_work_cancel_delayable(&lease_work);
    atomic_clear(&link_ready); atomic_clear(&connecting); discovering=false; handshaking=false;
    prompt(false); atomic_clear(&read_pending);
    k_work_cancel_delayable(&approval_work); k_work_cancel_delayable(&candidate_timeout);k_work_cancel_delayable(&ready_work);
    bt_conn_unref(old);
    k_work_cancel_delayable(&discover_work);
    k_msgq_purge(&packets); atomic_set(&clear_pending,1); k_work_submit(&receive_work);
    atomic_clear(&tearing_down);
    k_work_reschedule(&retry_work,K_MSEC(100));
}
/* Recycled is later than disconnected: controller/host slots are now reusable.
 * A fast keyboard swap must not depend on the previous attempt's software flag. */
static void relay_recycled(void) {
    if(!source && !atomic_get(&connecting) && !atomic_get(&tearing_down))k_work_reschedule(&retry_work,K_MSEC(100));
}
static void supervise_link(struct k_work *work) {
    ARG_UNUSED(work);
    k_spinlock_key_t key=k_spin_lock(&source_lock);
    struct bt_conn *conn=source?bt_conn_ref(source):NULL;
    k_spin_unlock(&source_lock,key);
    if(conn){
        struct bt_conn_info info;
        if(!bt_conn_get_info(conn,&info) && info.state==BT_CONN_STATE_DISCONNECTED)
            disconnected(conn,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(conn);
    } else if(!atomic_get(&connecting) && !atomic_get(&tearing_down)) {
        k_work_reschedule(&retry_work,K_NO_WAIT);
    }
    k_work_reschedule(&supervise_work,K_SECONDS(1));
}
BT_CONN_CB_DEFINE(relay_receiver_connections)={.connected=connected,.disconnected=disconnected,.security_changed=security_changed,.recycled=relay_recycled};

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
    k_work_reschedule(&supervise_work,K_SECONDS(1));
    k_work_reschedule(&retry_work,K_NO_WAIT);
}

/* Read-only troubleshooting snapshot; approximate concurrent counters, no flash writes. */
void zmk_receiver_relay_diagnostics(uint8_t out[24]) {
    memset(out,0,24);out[0]=1;
    out[1]=(source?1:0)|(scanning?2:0)|(atomic_get(&connecting)?4:0)|(atomic_get(&link_ready)?8:0);
    out[2]=k_msgq_num_used_get(&packets);
    sys_put_le32(atomic_get(&diag_received),out+4);sys_put_le32(atomic_get(&diag_processed),out+8);
    sys_put_le32(atomic_get(&diag_dropped),out+12);sys_put_le32(atomic_get(&diag_max_queue_ms),out+16);
    k_spinlock_key_t key=k_spin_lock(&source_lock);
    struct bt_conn *conn=source?bt_conn_ref(source):NULL;
    k_spin_unlock(&source_lock,key);
    if(conn){struct bt_conn_info info;if(!bt_conn_get_info(conn,&info)){
        sys_put_le16(info.le.interval,out+20);sys_put_le16(info.le.latency,out+22);
    }bt_conn_unref(conn);}
}

int zmk_receiver_request_source_diagnostics(void) {
    k_spinlock_key_t key=k_spin_lock(&source_diag_lock);source_diag_valid=false;k_spin_unlock(&source_diag_lock,key);
    key=k_spin_lock(&source_lock);struct bt_conn *conn=source && atomic_get(&link_ready)?bt_conn_ref(source):NULL;uint16_t handle=control_handle;k_spin_unlock(&source_lock,key);
    if(!conn)return -ENOTCONN;
    uint8_t command=0x84;int ret=bt_gatt_write_without_response(conn,handle,&command,1,false);bt_conn_unref(conn);return ret;
}
int zmk_receiver_source_diagnostics(uint8_t out[23]) {
    k_spinlock_key_t key=k_spin_lock(&source_diag_lock);
    if(!source_diag_valid){k_spin_unlock(&source_diag_lock,key);return -ENODATA;}
    memcpy(out,source_diag,19);sys_put_le32(k_uptime_get_32()-source_diag_at,out+19);
    k_spin_unlock(&source_diag_lock,key);return 0;
}
